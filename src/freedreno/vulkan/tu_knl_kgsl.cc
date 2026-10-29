/*
 * Copyright © 2020 Google, Inc.
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <poll.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>

#define __user
#include "ion/ion.h"
#include "ion/ion_4.19.h"
#include "msm_kgsl.h"

#include "util/libsync.h"
#include "util/simple_mtx.h"
#include "util/os_file.h"
#include "util/timespec.h"
#include "util/u_debug.h"
#include "util/u_vector.h"
#include "vk_util.h"

#include "tu_cmd_buffer.h"
#include "tu_cs.h"
#include "tu_device.h"
#include "tu_dynamic_rendering.h"
#include "tu_knl.h"
#include "tu_queue.h"
#include "tu_query_pool.h"
#include "tu_rmv.h"

/* ION_HEAP(ION_SYSTEM_HEAP_ID) */
#define KGSL_ION_SYSTEM_HEAP_MASK (1u << 25)


static int
safe_ioctl(int fd, unsigned long request, void *arg)
{
   int ret;

   do {
      ret = ioctl(fd, request, arg);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   return ret;
}

/* TU_KGSL_IGNORE_FAULTS=1: when KGSL reports that the context faulted
 * (EPROTO on submit/wait, or a guilty reset status) but the context is still
 * usable, keep going instead of reporting VK_ERROR_DEVICE_LOST. The kernel
 * has already recovered the GPU at that point.
 */
static bool
kgsl_ignore_faults(void)
{
   static int v = -1;
   if (unlikely(v < 0))
      v = debug_get_bool_option("TU_KGSL_IGNORE_FAULTS", false) ||
          debug_get_bool_option("TU_CTX_RECOVER", false);
   return v;
}

/* GPU clock experiments (DVFS hypothesis):
 *   TU_KGSL_FREQ=<mhz> or <min>-<max>  KGSL_PROP_DCVS_PROFILE min/max GPU
 *                                      frequency for this process (pin by
 *                                      giving one value).
 *   TU_KGSL_PWR=max|min|<2..100>       per-context KGSL_PROP_PWR_CONSTRAINT,
 *                                      applied to every submission.
 */
static int
kgsl_pwr_level(void)
{
   static int lvl = -2;
   if (lvl == -2) {
      const char *v = os_get_option("TU_KGSL_PWR");
      if (!v || !*v)
         lvl = -1;
      else if (!strcmp(v, "max"))
         lvl = KGSL_CONSTRAINT_PWR_MAX;
      else if (!strcmp(v, "min"))
         lvl = KGSL_CONSTRAINT_PWR_MIN;
      else
         lvl = CLAMP(atoi(v), KGSL_CONSTRAINT_PWR_PERC_MIN, KGSL_CONSTRAINT_PWR_PERC_MAX);
   }
   return lvl;
}

static void
kgsl_apply_power_settings(struct tu_device *dev, uint32_t ctx)
{
   int fd = dev->physical_device->local_fd;

   static bool freq_done = false;
   const char *f = os_get_option("TU_KGSL_FREQ");
   if (f && *f && !freq_done) {
      freq_done = true;
      unsigned lo = 0, hi = 0;
      if (sscanf(f, "%u-%u", &lo, &hi) != 2)
         hi = lo = (unsigned) atoi(f);
      struct kgsl_dcvs_attrs attrs;
      memset(&attrs, 0xff, sizeof(attrs)); /* KGSL_DCVS_ATTR_UNUSED */
      attrs.min_gpu_freq = lo;
      attrs.max_gpu_freq = hi;
      struct kgsl_dcvs_profile prof = {
         .attrs = (uint64_t)(uintptr_t)&attrs,
         .attrs_size = sizeof(attrs),
      };
      struct kgsl_device_getproperty sp = {
         .type = KGSL_PROP_DCVS_PROFILE,
         .value = &prof,
         .sizebytes = sizeof(prof),
      };
      int r = safe_ioctl(fd, IOCTL_KGSL_SETPROPERTY, &sp);
      mesa_logw("TU_KGSL_FREQ: DCVS profile min %u max %u MHz -> %s",
                lo, hi, r ? strerror(errno) : "ok");
   }

   int lvl = kgsl_pwr_level();
   if (lvl >= 0) {
      struct kgsl_device_constraint_pwrlevel pl = { .level = (unsigned) lvl };
      struct kgsl_device_constraint c = {
         .type = KGSL_CONSTRAINT_PWRLEVEL,
         .context_id = ctx,
         .data = &pl,
         .size = sizeof(pl),
      };
      struct kgsl_device_getproperty sp = {
         .type = KGSL_PROP_PWR_CONSTRAINT,
         .value = &c,
         .sizebytes = sizeof(c),
      };
      int r = safe_ioctl(fd, IOCTL_KGSL_SETPROPERTY, &sp);
      mesa_logw("TU_KGSL_PWR: ctx %u power constraint level %d -> %s",
                ctx, lvl, r ? strerror(errno) : "ok");
   }
}

static int
kgsl_submitqueue_new(struct tu_device *dev, struct tu_queue *queue)
{
   struct kgsl_drawctxt_create req = {
      .flags = KGSL_CONTEXT_SAVE_GMEM |
              KGSL_CONTEXT_NO_GMEM_ALLOC |
              KGSL_CONTEXT_PREAMBLE,
   };

   /* Diagnostics for GPU hangs on KGSL: tweak context creation.
    *   TU_KGSL_PREEMPT=ring|fine     preemption style (default: kernel default)
    *   TU_KGSL_CTX_TYPE=vk           tag the context as Vulkan
    *   TU_KGSL_CTX_FLAGS=0x...       OR raw KGSL_CONTEXT_* flags
    */
   {
      const char *pre = getenv("TU_KGSL_PREEMPT");
      if (pre && !strcmp(pre, "ring"))
         req.flags |= KGSL_CONTEXT_PREEMPT_STYLE_RINGBUFFER
                      << KGSL_CONTEXT_PREEMPT_STYLE_SHIFT;
      else if (pre && !strcmp(pre, "fine"))
         req.flags |= KGSL_CONTEXT_PREEMPT_STYLE_FINEGRAIN
                      << KGSL_CONTEXT_PREEMPT_STYLE_SHIFT;
      const char *type = getenv("TU_KGSL_CTX_TYPE");
      if (type && !strcmp(type, "vk"))
         req.flags |= KGSL_CONTEXT_TYPE_VK << KGSL_CONTEXT_TYPE_SHIFT;
      const char *raw = getenv("TU_KGSL_CTX_FLAGS");
      if (raw)
         req.flags |= (uint32_t) strtoul(raw, NULL, 0);
      if (kgsl_pwr_level() >= 0)
         req.flags |= KGSL_CONTEXT_PWR_CONSTRAINT;
      if (pre || type || raw)
         mesa_logi("kgsl: drawctxt flags 0x%x", req.flags);
   }

   int ret;
   if (tu_hanglog_enabled) {
      struct kgsl_drawctxt_create req2 = req;
      req2.flags |= KGSL_CONTEXT_FAULT_INFO;
      ret = safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_DRAWCTXT_CREATE, &req2);
      if (!ret) {
         mesa_logw("TU_HANGLOG: context %u created with FAULT_INFO", req2.drawctxt_id);
         queue->msm_queue_id = req2.drawctxt_id;
         kgsl_apply_power_settings(dev, queue->msm_queue_id);
         return 0;
      }
      mesa_logw("TU_HANGLOG: FAULT_INFO context flag rejected (%s), continuing without",
                strerror(errno));
   }

   ret = safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_DRAWCTXT_CREATE, &req);
   if (ret)
      return ret;

   queue->msm_queue_id = req.drawctxt_id;
   kgsl_apply_power_settings(dev, queue->msm_queue_id);

   return 0;
}

static void
kgsl_submitqueue_close(struct tu_device *dev, struct tu_queue *queue)
{
   struct kgsl_drawctxt_destroy req = {
      .drawctxt_id = queue->msm_queue_id,
   };

   safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_DRAWCTXT_DESTROY, &req);
}

static void kgsl_bo_finish(struct tu_device *dev, struct tu_bo *bo);

static VkResult
bo_init_new_dmaheap(struct tu_device *dev, struct tu_bo **out_bo, uint64_t size,
                uint64_t align, enum tu_bo_alloc_flags flags)
{
   struct dma_heap_allocation_data alloc = {
      .len = size,
      .fd_flags = O_RDWR | O_CLOEXEC,
   };

   int ret;
   ret = safe_ioctl(dev->physical_device->kgsl_dma_fd, DMA_HEAP_IOCTL_ALLOC,
                    &alloc);

   if (ret) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "DMA_HEAP_IOCTL_ALLOC failed (%s)", strerror(errno));
   }

   /* tu_bo_init_dmabuf() only borrows the fd: it keeps a dup of its own in
    * bo->shared_fd, which is what kgsl_bo_finish() closes. That is correct for
    * an imported dma-buf, whose fd belongs to the caller, but here the fd is
    * ours and nobody else will ever drop it. Leaking it keeps the dma-buf
    * alive after the BO is destroyed, so the memory is never reclaimed.
    */
   VkResult result =
      tu_bo_init_dmabuf(dev, out_bo, -1, align, TU_BO_ALLOC_NO_FLAGS, alloc.fd);
   close(alloc.fd);

   return result;
}

static VkResult
bo_init_new_ion(struct tu_device *dev, struct tu_bo **out_bo, uint64_t size,
                uint64_t align, enum tu_bo_alloc_flags flags)
{
   struct ion_new_allocation_data alloc = {
      .len = size,
      .heap_id_mask = KGSL_ION_SYSTEM_HEAP_MASK,
      .flags = 0,
      .fd = -1,
   };

   int ret;
   ret = safe_ioctl(dev->physical_device->kgsl_dma_fd, ION_IOC_NEW_ALLOC, &alloc);
   if (ret) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "ION_IOC_NEW_ALLOC failed (%s)", strerror(errno));
   }

   /* See bo_init_new_dmaheap(): the fd is ours to close. */
   VkResult result =
      tu_bo_init_dmabuf(dev, out_bo, -1, align, TU_BO_ALLOC_NO_FLAGS, alloc.fd);
   close(alloc.fd);

   return result;
}

static VkResult
bo_init_new_ion_legacy(struct tu_device *dev, struct tu_bo **out_bo, uint64_t size,
                       uint64_t align, enum tu_bo_alloc_flags flags)
{
   struct ion_allocation_data alloc = {
      .len = size,
      .align = 4096,
      .heap_id_mask = KGSL_ION_SYSTEM_HEAP_MASK,
      .flags = 0,
      .handle = -1,
   };

   int ret;
   ret = safe_ioctl(dev->physical_device->kgsl_dma_fd, ION_IOC_ALLOC, &alloc);
   if (ret) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "ION_IOC_ALLOC failed (%s)", strerror(errno));
   }

   struct ion_fd_data share = {
      .handle = alloc.handle,
      .fd = -1,
   };

   ret = safe_ioctl(dev->physical_device->kgsl_dma_fd, ION_IOC_SHARE, &share);
   if (ret) {
      struct ion_handle_data free = {
         .handle = alloc.handle,
      };
      safe_ioctl(dev->physical_device->kgsl_dma_fd, ION_IOC_FREE, &free);

      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "ION_IOC_SHARE failed (%s)", strerror(errno));
   }

   struct ion_handle_data free = {
      .handle = alloc.handle,
   };
   ret = safe_ioctl(dev->physical_device->kgsl_dma_fd, ION_IOC_FREE, &free);
   if (ret) {
      close(share.fd);
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "ION_IOC_FREE failed (%s)", strerror(errno));
   }

   /* See bo_init_new_dmaheap(): the fd is ours to close. */
   VkResult result =
      tu_bo_init_dmabuf(dev, out_bo, -1, align, TU_BO_ALLOC_NO_FLAGS, share.fd);
   close(share.fd);

   return result;
}

static VkResult
kgsl_bo_user_map(struct tu_device *dev, struct tu_bo *bo, uint64_t client_iova)
{
   uint64_t offset = bo->gem_handle << 12;
   /* KGSL may free the GPU VA of a destroyed BO asynchronously, and without MAP_FIXED_NOREPLACE,
    * it then silently places the BO elsewhere. With MAP_FIXED_NOREPLACE, KGSL flushes pending frees
    * and retries instead. Kernels older than 4.17 ignore the flag and treat client_iova as a hint.
    */
   int flags = MAP_SHARED | (client_iova ? MAP_FIXED_NOREPLACE : 0);
   void *map = mmap(reinterpret_cast<void *>(client_iova), bo->size, PROT_READ | PROT_WRITE,
                    flags, dev->physical_device->local_fd, offset);
   if (map == MAP_FAILED) {
      int err = errno;
      kgsl_bo_finish(dev, bo);

      if (client_iova && err == EEXIST) {
         return vk_errorf(dev, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "mmap could not map the given address");
      }

      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "mmap failed (%s)", strerror(err));
   }

   if (client_iova && reinterpret_cast<uint64_t>(map) != client_iova) {
      munmap(map, bo->size);
      kgsl_bo_finish(dev, bo);

      return vk_errorf(dev, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                       "mmap could not map the given address");
   }

   bo->map = map;
   bo->iova = reinterpret_cast<uint64_t>(map);

   /* Because we're using SVM, the CPU mapping and GPU mapping are the same
    * and the CPU mapping must stay fixed for the lifetime of the BO.
    */
   bo->never_unmap = true;

   return VK_SUCCESS;
}

static VkResult
kgsl_sparse_vma_map(struct tu_device *dev,
                    struct tu_sparse_vma *vma,
                    struct tu_bo *bo, uint64_t bo_offset)
{
   struct kgsl_gpumem_bind_range range = {
      .child_offset = bo_offset,
      .target_offset = 0,
      .length = vma->kgsl.virtual_bo->size,
      .child_id = bo->gem_handle,
      .op = KGSL_GPUMEM_RANGE_OP_BIND,
   };

   struct kgsl_gpumem_bind_ranges req = {
      .ranges = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&range)),
      .ranges_nents = 1,
      .ranges_size = sizeof(range),
      .id = vma->kgsl.virtual_bo->gem_handle,
      .flags = 0,
   };

   int ret;

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUMEM_BIND_RANGES, &req);
   if (ret) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "GPUMEM_BIND_RANGES failed (%s)", strerror(errno));
   }

   return VK_SUCCESS;
}

static uint32_t
kgsl_align_flag(uint64_t align)
{
   if (align > os_page_size) {
      /* kgsl treats the alignment as a hint and will round it down, but
       * at least with current behavior, a 64K alignment and 1 MB alignment
       * are respected.
       */
      if (align <= (1 << 16)) { /* 64K */
         return 16 << KGSL_MEMALIGN_SHIFT;
      } else {
         assert(align <= (1 << 20)); /* 1 MB */
         return 20 << KGSL_MEMALIGN_SHIFT;
      }
   }

   return 0;
}

static void hanglog_query_faults(struct tu_device *dev, uint32_t ctx);

/* On A8XX, freeing IB memory with a plain GPUMEM_FREE_ID leads to GPU hangs,
 * even when the GPU is idle. Fetching IBs through a virtual BO alias whose
 * range is unbound before the memory is freed avoids them.
 */
static bool
kgsl_bo_needs_alias(struct tu_device *dev, enum tu_bo_alloc_flags flags)
{
   static int nofix = -1;
   if (unlikely(nofix < 0)) {
      const char *v = os_get_option("TU_A8XX_NOFIX");
      nofix = v && strstr(v, "alias");
   }
   return !nofix && (flags & TU_BO_ALLOC_NO_32B_ROLLOVER) &&
          dev->physical_device->has_sparse &&
          dev->physical_device->info->chip >= A8XX;
}

static VkResult
kgsl_bo_create_alias(struct tu_device *dev, struct tu_bo *bo, uint64_t align)
{
   int fd = dev->physical_device->local_fd;

   struct kgsl_gpuobj_alloc alloc = {
      .size = bo->size,
      .flags = KGSL_MEMFLAGS_VBO | KGSL_MEMFLAGS_VBO_NO_MAP_ZERO |
               kgsl_align_flag(align),
   };

   if (safe_ioctl(fd, IOCTL_KGSL_GPUOBJ_ALLOC, &alloc)) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "GPUOBJ_ALLOC failed (%s)", strerror(errno));
   }

   struct kgsl_gpumem_get_info info = {
      .id = alloc.id,
   };

   struct kgsl_gpumem_bind_range range = {
      .length = bo->size,
      .child_id = bo->gem_handle,
      .op = KGSL_GPUMEM_RANGE_OP_BIND,
   };

   struct kgsl_gpumem_bind_ranges bind = {
      .ranges = (uint64_t)(uintptr_t)&range,
      .ranges_nents = 1,
      .ranges_size = sizeof(range),
      .id = alloc.id,
   };

   if (safe_ioctl(fd, IOCTL_KGSL_GPUMEM_GET_INFO, &info) ||
       safe_ioctl(fd, IOCTL_KGSL_GPUMEM_BIND_RANGES, &bind)) {
      VkResult result =
         vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                   "binding IB alias failed (%s)", strerror(errno));

      struct kgsl_gpumem_free_id req = {
         .id = alloc.id,
      };
      safe_ioctl(fd, IOCTL_KGSL_GPUMEM_FREE_ID, &req);
      return result;
   }

   bo->alias_handle = alloc.id;
   bo->iova = info.gpuaddr;
   return VK_SUCCESS;
}

static void
kgsl_bo_destroy_alias(struct tu_device *dev, struct tu_bo *bo)
{
   int fd = dev->physical_device->local_fd;

   struct kgsl_gpumem_bind_range range = {
      .length = bo->size,
      .child_id = bo->gem_handle,
      .op = KGSL_GPUMEM_RANGE_OP_UNBIND,
   };

   struct kgsl_gpumem_bind_ranges unbind = {
      .ranges = (uint64_t)(uintptr_t)&range,
      .ranges_nents = 1,
      .ranges_size = sizeof(range),
      .id = bo->alias_handle,
   };

   safe_ioctl(fd, IOCTL_KGSL_GPUMEM_BIND_RANGES, &unbind);

   struct kgsl_gpumem_free_id req = {
      .id = bo->alias_handle,
   };
   safe_ioctl(fd, IOCTL_KGSL_GPUMEM_FREE_ID, &req);
}


static VkResult
kgsl_bo_init(struct tu_device *dev,
             struct vk_object_base *base,
             struct tu_bo **out_bo,
             uint64_t size,
             uint64_t align,
             uint64_t client_iova,
             VkMemoryPropertyFlags mem_property,
             enum tu_bo_alloc_flags flags,
             struct tu_sparse_vma *lazy_vma,
             const char *name)
{
   if (flags & TU_BO_ALLOC_SHAREABLE) {
      /* The Vulkan spec doesn't forbid allocating exportable memory with a
       * fixed address, only imported memory, but on kgsl we can't sensibly
       * implement it so just always reject it.
       */
      if (client_iova) {
         return vk_errorf(dev, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "cannot allocate an exportable BO with a fixed address");
      }

      switch(dev->physical_device->kgsl_dma_type) {
      case TU_KGSL_DMA_TYPE_DMAHEAP:
         return bo_init_new_dmaheap(dev, out_bo, size, align, flags);
      case TU_KGSL_DMA_TYPE_ION:
         return bo_init_new_ion(dev, out_bo, size, align, flags);
      case TU_KGSL_DMA_TYPE_ION_LEGACY:
         return bo_init_new_ion_legacy(dev, out_bo, size, align, flags);
      }
   }

   struct kgsl_gpumem_alloc_id req = {
      .size = size,
   };

   if (mem_property & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
      if (mem_property & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
         req.flags |= KGSL_MEMFLAGS_IOCOHERENT;
      }

      req.flags |= KGSL_CACHEMODE_WRITEBACK << KGSL_CACHEMODE_SHIFT;
   } else {
      req.flags |= KGSL_CACHEMODE_WRITECOMBINE << KGSL_CACHEMODE_SHIFT;
   }

   if (flags & TU_BO_ALLOC_GPU_READ_ONLY)
      req.flags |= KGSL_MEMFLAGS_GPUREADONLY;

   if (flags & TU_BO_ALLOC_REPLAYABLE)
      req.flags |= KGSL_MEMFLAGS_USE_CPU_MAP;

   req.flags |= kgsl_align_flag(align);

   /* Forcing everything in the first 4GB is overkill, but we don't have many
    * other options here since we don't control iova addresses and kgsl
    * doesn't have a flag to avoid 32B rollover.
    */
   if (flags & TU_BO_ALLOC_NO_32B_ROLLOVER)
      req.flags |= KGSL_MEMFLAGS_FORCE_32BIT;

   int ret;

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUMEM_ALLOC_ID, &req);
   if (ret) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "GPUMEM_ALLOC_ID failed (%s)", strerror(errno));
   }

   struct tu_bo* bo = tu_device_lookup_bo(dev, req.id);
   assert(bo && bo->gem_handle == 0);

   *bo = (struct tu_bo) {
      .gem_handle = req.id,
      .size = req.mmapsize,
      .iova = req.gpuaddr,
      .name = tu_debug_bos_add(dev, req.mmapsize, name),
      .refcnt = 1,
      .shared_fd = -1,
      .base = base,
   };

   tu_dump_bo_init(dev, bo);

   VkResult result = VK_SUCCESS;

   if (lazy_vma) {
      result = kgsl_sparse_vma_map(dev, lazy_vma, bo, 0);
   } else if (flags & TU_BO_ALLOC_REPLAYABLE) {
      result = kgsl_bo_user_map(dev, bo, client_iova);
   } else if (kgsl_bo_needs_alias(dev, flags)) {
      result = kgsl_bo_create_alias(dev, bo, align);
      if (result != VK_SUCCESS)
         kgsl_bo_finish(dev, bo);
   }

   if (result != VK_SUCCESS)
      return result;

   *out_bo = bo;

   if (unlikely(tu_hanglog_enabled))
      tu_hanglog_bo_event(1, bo->iova, bo->size, name);

   TU_RMV(bo_allocate, dev, bo);
   if (flags & TU_BO_ALLOC_INTERNAL_RESOURCE) {
      TU_RMV(internal_resource_create, dev, bo);
      TU_RMV(resource_name, dev, bo, name);
   }

   return VK_SUCCESS;
}

static VkResult
kgsl_bo_init_dmabuf(struct tu_device *dev,
                    struct tu_bo **out_bo,
                    uint64_t size,
                    uint64_t align,
                    enum tu_bo_alloc_flags flags,
                    int fd)
{
   struct kgsl_gpuobj_import_dma_buf import_dmabuf = {
      .fd = fd,
   };
   struct kgsl_gpuobj_import req = {
      .priv = reinterpret_cast<uintptr_t>(&import_dmabuf),
      .priv_len = sizeof(import_dmabuf),
      .flags = 0,
      .type = KGSL_USER_MEM_TYPE_DMABUF,
   };
   int ret;

   req.flags |= kgsl_align_flag(align);

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUOBJ_IMPORT, &req);
   if (ret)
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to import dma-buf (%s)\n", strerror(errno));

   struct kgsl_gpuobj_info info_req = {
      .id = req.id,
   };

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUOBJ_INFO, &info_req);
   if (ret) {
      struct kgsl_gpumem_free_id free_req = {
         .id = req.id,
      };
      safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_GPUMEM_FREE_ID,
                 &free_req);

      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to get dma-buf info (%s)\n", strerror(errno));
   }

   struct tu_bo* bo = tu_device_lookup_bo(dev, req.id);
   assert(bo && bo->gem_handle == 0);

   *bo = (struct tu_bo) {
      .gem_handle = req.id,
      .size = info_req.size,
      .iova = info_req.gpuaddr,
      .name = tu_debug_bos_add(dev, info_req.size, "dmabuf"),
      .refcnt = 1,
      .shared_fd = os_dupfd_cloexec(fd),
   };

   struct stat st;
   if (fstat(fd, &st) == 0)
      /* Use the inode number as the unique ID, but set the MSB to avoid
       * collisions with 32-bit KGSL handles (which are used for native BOs).
       */
      bo->unique_id = st.st_ino | (1ULL << 63);

   tu_dump_bo_init(dev, bo);

   *out_bo = bo;
   if (unlikely(tu_hanglog_enabled))
      tu_hanglog_bo_event(3, bo->iova, bo->size, "dmabuf");

   return VK_SUCCESS;
}

static int
kgsl_bo_export_dmabuf(struct tu_device *dev, struct tu_bo *bo)
{
   assert(bo->shared_fd != -1);
   return os_dupfd_cloexec(bo->shared_fd);
}

static VkResult
kgsl_bo_map(struct tu_device *dev, struct tu_bo *bo, void *placed_addr)
{
   void *map = MAP_FAILED;
   if (bo->shared_fd == -1) {
      uint64_t offset = bo->gem_handle << 12;
      map = mmap(placed_addr, bo->size, PROT_READ | PROT_WRITE,
                 MAP_SHARED | (placed_addr != NULL ? MAP_FIXED : 0),
                 dev->physical_device->local_fd, offset);
   } else {
      map = mmap(placed_addr, bo->size, PROT_READ | PROT_WRITE,
                 MAP_SHARED | (placed_addr != NULL ? MAP_FIXED : 0),
                 bo->shared_fd, 0);
   }

   if (map == MAP_FAILED)
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);

   bo->map = map;
   TU_RMV(bo_map, dev, bo);

   return VK_SUCCESS;
}

static void
kgsl_bo_allow_dump(struct tu_device *dev, struct tu_bo *bo)
{
}

static void
kgsl_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   assert(bo->gem_handle);

   if (!p_atomic_dec_zero(&bo->refcnt))
      return;

   tu_debug_bos_del(dev, bo);
   tu_dump_bo_del(dev, bo);
   if (unlikely(tu_hanglog_enabled))
      tu_hanglog_bo_event(2, bo->iova, bo->size, NULL);

   if (bo->map) {
      TU_RMV(bo_unmap, dev, bo);
      munmap(bo->map, bo->size);
   }

   if (bo->shared_fd != -1)
      close(bo->shared_fd);

   TU_RMV(bo_destroy, dev, bo);

   if (bo->alias_handle)
      kgsl_bo_destroy_alias(dev, bo);

   struct kgsl_gpumem_free_id req = {
      .id = bo->gem_handle
   };

   /* Tell sparse array that entry is free */
   memset(bo, 0, sizeof(*bo));

   safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_GPUMEM_FREE_ID, &req);
}

static VkResult
kgsl_sparse_vma_init(struct tu_device *dev,
                     struct vk_object_base *base,
                     struct tu_sparse_vma *out_vma,
                     uint64_t *out_iova,
                     enum tu_sparse_vma_flags flags,
                     uint64_t size, uint64_t align, uint64_t client_iova)
{
   /* Note: we cannot use kgsl_gpumem_alloc_id because it only has a 32-bit
    * flags value. kgsl_gpuobj_alloc seems to be the only ioctl we can use.
    */
   struct kgsl_gpuobj_alloc req = {
      .size = size,
      .flags = KGSL_MEMFLAGS_VBO,
      .va_len = 0, /* seems to be unused? */
   };

   if (flags & TU_SPARSE_VMA_REPLAYABLE)
      req.flags |= KGSL_MEMFLAGS_USE_CPU_MAP;

   if (!(flags & TU_SPARSE_VMA_MAP_ZERO))
      req.flags |= KGSL_MEMFLAGS_VBO_NO_MAP_ZERO;

   req.flags |= kgsl_align_flag(align);

   int ret;

   ret = safe_ioctl(dev->physical_device->local_fd,
                    IOCTL_KGSL_GPUOBJ_ALLOC, &req);
   if (ret) {
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "GPUOBJ_ALLOC failed (%s)", strerror(errno));
   }

   struct tu_bo *bo = tu_device_lookup_bo(dev, req.id);
   assert(bo && bo->gem_handle == 0);

   *bo = (struct tu_bo) {
      .gem_handle = req.id,
      .size = req.mmapsize,
      .name = NULL,
      .refcnt = 1,
      .shared_fd = -1,
      .base = base,
   };

   if (flags & TU_SPARSE_VMA_REPLAYABLE) {
      VkResult result = kgsl_bo_user_map(dev, bo, client_iova);
      if (result != VK_SUCCESS)
         return result;
   } else {
      /* For some cursed reason, the ioctl doesn't return the GPU address so
       * we have to query it.
       */
      struct kgsl_gpumem_get_info info = {
         .id = req.id,
      };

      ret = safe_ioctl(dev->physical_device->local_fd,
                       IOCTL_KGSL_GPUMEM_GET_INFO, &info);
      if (ret) {
         return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                          "GPUMEM_GET_INFO failed (%s)", strerror(errno));
      }

      bo->iova = info.gpuaddr;
   }

   out_vma->kgsl.virtual_bo = bo;
   *out_iova = bo->iova;
   return VK_SUCCESS;
}

static void
kgsl_sparse_vma_finish(struct tu_device *dev,
                       struct tu_sparse_vma *vma)
{
   struct kgsl_gpuobj_free req = {
      .id = vma->kgsl.virtual_bo->gem_handle
   };

   /* Tell sparse array that entry is free */
   memset(vma->kgsl.virtual_bo, 0, sizeof(*vma->kgsl.virtual_bo));

   safe_ioctl(dev->physical_device->local_fd, IOCTL_KGSL_GPUOBJ_FREE, &req);
}

static VkResult
get_kgsl_prop(int fd, unsigned int type, void *value, size_t size)
{
   struct kgsl_device_getproperty getprop = {
      .type = type,
      .value = value,
      .sizebytes = size,
   };

   return safe_ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &getprop)
             ? VK_ERROR_UNKNOWN
             : VK_SUCCESS;
}

static bool
kgsl_is_memory_type_supported(int fd, uint32_t flags)
{
   struct kgsl_gpumem_alloc_id req_alloc = {
      .flags = flags,
      .size = 0x1000,
   };

   int ret = safe_ioctl(fd, IOCTL_KGSL_GPUMEM_ALLOC_ID, &req_alloc);
   if (ret) {
      return false;
   }

   /* The kernel echoes back the *actual* flags it used. Some KGSL
    * versions silently strip unsupported flags (e.g. IOCOHERENT on
    * GPUs that lack IO-coherence) instead of failing the ioctl.
    * Detect this by checking the requested bits are still present.
    */
   bool supported = (req_alloc.flags & flags) == flags;

   struct kgsl_gpumem_free_id req_free = { .id = req_alloc.id };

   safe_ioctl(fd, IOCTL_KGSL_GPUMEM_FREE_ID, &req_free);

   return supported;
}

static bool
kgsl_is_virtual_bo_supported(int fd)
{
   bool supported = false;
   struct kgsl_gpuobj_alloc req_alloc_parent = {
      .size = 0x2000,
      .flags = KGSL_MEMFLAGS_VBO,
   };
   struct kgsl_gpumem_alloc_id req_alloc_child = {
      .size = 0x1000,
   };
   struct kgsl_gpumem_bind_range req_range = {};
   struct kgsl_gpumem_bind_ranges req_ranges = {};
   struct kgsl_gpumem_free_id req_free_child = {};
   struct kgsl_gpuobj_free req_free_parent = {};

   int ret = safe_ioctl(fd, IOCTL_KGSL_GPUOBJ_ALLOC, &req_alloc_parent);
   if (ret) {
      return false;
   }

   ret = safe_ioctl(fd, IOCTL_KGSL_GPUMEM_ALLOC_ID, &req_alloc_child);
   if (ret) {
      goto free_parent;
   }

   req_range = {
      .child_offset = 0,
      .target_offset = 0,
      .length = 0x1000,
      .child_id = req_alloc_child.id,
      .op = KGSL_GPUMEM_RANGE_OP_BIND,
   };

   req_ranges = {
      .ranges = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&req_range)),
      .ranges_nents = 1,
      .ranges_size = sizeof(req_range),
      .id = req_alloc_parent.id,
      .flags = 0,
   };

   ret = safe_ioctl(fd, IOCTL_KGSL_GPUMEM_BIND_RANGES, &req_ranges);
   if (ret) {
      goto free_child;
   }

   supported = true;

free_child:
   req_free_child = { .id = req_alloc_child.id };
   safe_ioctl(fd, IOCTL_KGSL_GPUMEM_FREE_ID, &req_free_child);

free_parent:
   req_free_parent = { .id = req_alloc_parent.id };
   safe_ioctl(fd, IOCTL_KGSL_GPUOBJ_FREE, &req_free_parent);

   return supported;
}

enum kgsl_syncobj_state {
   KGSL_SYNCOBJ_STATE_UNSIGNALED,
   KGSL_SYNCOBJ_STATE_SIGNALED,
   KGSL_SYNCOBJ_STATE_TS,
   KGSL_SYNCOBJ_STATE_FD,
};

struct kgsl_syncobj
{
   struct vk_object_base base;
   enum kgsl_syncobj_state state;

   struct tu_queue *queue;
   uint32_t timestamp;

   int fd;
};

static void
kgsl_syncobj_init(struct kgsl_syncobj *s, bool signaled)
{
   s->state =
      signaled ? KGSL_SYNCOBJ_STATE_SIGNALED : KGSL_SYNCOBJ_STATE_UNSIGNALED;

   s->timestamp = UINT32_MAX;
   s->fd = -1;
}

static void
kgsl_syncobj_reset(struct kgsl_syncobj *s)
{
   if (s->state == KGSL_SYNCOBJ_STATE_FD && s->fd >= 0) {
      ASSERTED int ret = close(s->fd);
      assert(ret == 0);
   }

   s->state = KGSL_SYNCOBJ_STATE_UNSIGNALED;
   s->timestamp = UINT32_MAX;
   s->fd = -1;
}

static void
kgsl_syncobj_destroy(struct kgsl_syncobj *s)
{
   kgsl_syncobj_reset(s);
}

static VkResult
kgsl_sync_error(struct tu_device *device, const char *operation, int error)
{
   VkResult result = VK_ERROR_UNKNOWN;
   if (error == ENOMEM)
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
   else if (error == EMFILE || error == ENFILE)
      result = VK_ERROR_TOO_MANY_OBJECTS;

   return vk_errorf(device, result, "KGSL %s failed: %s", operation, strerror(error));
}

static VkResult
kgsl_syncobj_dup(struct tu_device *device, const struct kgsl_syncobj *s, struct kgsl_syncobj *out)
{
   struct kgsl_syncobj copy = *s;
   if (s->state == KGSL_SYNCOBJ_STATE_FD) {
      copy.fd = os_dupfd_cloexec(s->fd);
      if (copy.fd < 0)
         return kgsl_sync_error(device, "sync FD duplication", errno);
   }
   *out = copy;
   return VK_SUCCESS;
}

static bool kgsl_ts_xlate(uint32_t ctx, uint32_t *ts);

static int
timestamp_to_fd(struct tu_queue *queue, uint32_t timestamp)
{
   int fd;
   if (kgsl_ts_xlate(queue->msm_queue_id, &timestamp))
      timestamp = 0; /* already signaled */
   struct kgsl_timestamp_event event = {
      .type = KGSL_TIMESTAMP_EVENT_FENCE,
      .timestamp = timestamp,
      .context_id = queue->msm_queue_id,
      .priv = &fd,
      .len = sizeof(fd),
   };

   int ret = safe_ioctl(queue->device->fd, IOCTL_KGSL_TIMESTAMP_EVENT, &event);
   if (ret)
      return -1;

   return fd;
}

static int
kgsl_syncobj_ts_to_fd(const struct kgsl_syncobj *syncobj)
{
   assert(syncobj->state == KGSL_SYNCOBJ_STATE_TS);
   return timestamp_to_fd(syncobj->queue, syncobj->timestamp);
}

/* return true if timestamp a is greater (more recent) then b
 * this relies on timestamps never having a difference > (1<<31)
 */
static inline bool
timestamp_cmp(uint32_t a, uint32_t b)
{
   return static_cast<int32_t>(a - b) >= 0;
}

static uint32_t
max_ts(uint32_t a, uint32_t b)
{
   return timestamp_cmp(a, b) ? a : b;
}

/* TU_CTX_RECOVER=1: when KGSL invalidates a context (e.g. "gpu fault
 * threshold exceeded"), create a new context and keep going instead of
 * reporting VK_ERROR_DEVICE_LOST. Timestamps handed out to the rest of the
 * driver are virtual: kernel timestamp + per-context offset. Anything at or
 * below a context's offset belongs to a previous (dead) context and is
 * treated as signaled.
 */
static bool
kgsl_ctx_recover_enabled(void)
{
   static int v = -1;
   if (unlikely(v < 0))
      v = debug_get_bool_option("TU_CTX_RECOVER", false);
   return v;
}

struct kgsl_ctx_xlate {
   uint32_t ctx;
   uint32_t offset;
   bool dead;
};
static struct kgsl_ctx_xlate kgsl_xlate[256];
static unsigned kgsl_nxlate;
static simple_mtx_t kgsl_xlate_mtx = SIMPLE_MTX_INITIALIZER;

static struct kgsl_ctx_xlate *
kgsl_xlate_find_locked(uint32_t ctx)
{
   for (unsigned i = 0; i < kgsl_nxlate; i++)
      if (kgsl_xlate[i].ctx == ctx)
         return &kgsl_xlate[i];
   return NULL;
}

/* Returns true if the (virtual) timestamp is already signaled; otherwise
 * converts it to the context's kernel timestamp.
 */
static bool
kgsl_ts_xlate(uint32_t ctx, uint32_t *ts)
{
   if (!kgsl_ctx_recover_enabled())
      return false;
   bool signaled = false;
   simple_mtx_lock(&kgsl_xlate_mtx);
   struct kgsl_ctx_xlate *x = kgsl_xlate_find_locked(ctx);
   if (x) {
      if (x->dead || !timestamp_cmp(*ts, x->offset + 1))
         signaled = true;
      else
         *ts -= x->offset;
   }
   simple_mtx_unlock(&kgsl_xlate_mtx);
   return signaled;
}

static uint32_t
kgsl_ctx_offset(uint32_t ctx)
{
   if (!kgsl_ctx_recover_enabled())
      return 0;
   simple_mtx_lock(&kgsl_xlate_mtx);
   struct kgsl_ctx_xlate *x = kgsl_xlate_find_locked(ctx);
   uint32_t off = x ? x->offset : 0;
   simple_mtx_unlock(&kgsl_xlate_mtx);
   return off;
}

static int
kgsl_queue_recover(struct tu_queue *queue)
{
   static simple_mtx_t recover_mtx = SIMPLE_MTX_INITIALIZER;
   simple_mtx_lock(&recover_mtx);
   const uint32_t old_ctx = queue->msm_queue_id;
   const uint32_t vbase = queue->fence >= 0 ? (uint32_t) queue->fence : 0;
   int r = kgsl_submitqueue_new(queue->device, queue);
   if (r) {
      queue->msm_queue_id = old_ctx;
      mesa_logw("TU_CTX_RECOVER: creating a new context failed (%s)", strerror(errno));
      simple_mtx_unlock(&recover_mtx);
      return r;
   }
   simple_mtx_lock(&kgsl_xlate_mtx);
   struct kgsl_ctx_xlate *o = kgsl_xlate_find_locked(old_ctx);
   if (!o && kgsl_nxlate < ARRAY_SIZE(kgsl_xlate)) {
      o = &kgsl_xlate[kgsl_nxlate++];
      o->ctx = old_ctx;
      o->offset = 0;
   }
   if (o)
      o->dead = true;
   if (kgsl_nxlate < ARRAY_SIZE(kgsl_xlate)) {
      struct kgsl_ctx_xlate *n = &kgsl_xlate[kgsl_nxlate++];
      n->ctx = queue->msm_queue_id;
      n->offset = vbase;
      n->dead = false;
   }
   simple_mtx_unlock(&kgsl_xlate_mtx);
   queue->safemode_faults = 0;
   static unsigned count = 0;
   mesa_logw("TU_CTX_RECOVER: ctx %u invalidated -> new ctx %u (timestamp base %u, recovery #%u)",
             old_ctx, queue->msm_queue_id, vbase, ++count);
   simple_mtx_unlock(&recover_mtx);
   return 0;
}

static uint32_t
min_ts(uint32_t a, uint32_t b)
{
   return timestamp_cmp(a, b) ? b : a;
}

static int
get_relative_ms(uint64_t abs_timeout_ns)
{
   if (abs_timeout_ns >= INT64_MAX)
      /* We can assume that a wait with a value this high is a forever wait
       * and return -1 here as it's the infinite timeout for ppoll() while
       * being the highest unsigned integer value for the wait KGSL IOCTL
       */
      return -1;

   uint64_t cur_time_ns = os_time_get_nano();
   if (abs_timeout_ns <= cur_time_ns)
      return 0;

   uint64_t relative_ns = abs_timeout_ns - cur_time_ns;
   uint64_t relative_ms = relative_ns / 1000000 + (relative_ns % 1000000 != 0);
   return relative_ms > INT_MAX ? -1 : relative_ms;
}

static VkResult
kgsl_timestamp_error(
   struct tu_device *device, unsigned int context_id, unsigned int timestamp, const char *operation, int error)
{
   if (error == ETIMEDOUT)
      return VK_TIMEOUT;

   /* - EPROTO - Device faulted since the last check;
    * - ENOENT - Context has been detached.
    */
   if (kgsl_ctx_recover_enabled() &&
       (error == EDEADLK || error == EPROTO || error == ENOENT || error == EINVAL)) {
      mesa_logw("TU_CTX_RECOVER: %s on ctx %u ts %u failed (%s) -> treated as signaled",
                operation, context_id, timestamp, strerror(error));
      return VK_SUCCESS;
   }
   if (error == EDEADLK || error == EPROTO || error == ENOENT)
      return vk_device_set_lost(&device->vk, "KGSL timestamp %s failed: context %u, timestamp %u, errno %d (%s)",
                                operation, context_id, timestamp, error, strerror(error));

   return vk_errorf(device, VK_ERROR_UNKNOWN, "KGSL timestamp %s failed: context %u, timestamp %u, errno %d (%s)",
                    operation, context_id, timestamp, error, strerror(error));
}

static VkResult
kgsl_poll_timestamp_raw(struct tu_device *device, unsigned int context_id, unsigned int timestamp)
{
   struct kgsl_cmdstream_readtimestamp_ctxtid read = {
      .context_id = context_id,
      .type = KGSL_TIMESTAMP_RETIRED,
   };

   if (safe_ioctl(device->fd, IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID, &read))
      return vk_errorf(device, VK_ERROR_UNKNOWN, "KGSL timestamp read failed: context %u, timestamp %u, errno %d (%s)",
                       context_id, timestamp, errno, strerror(errno));

   return timestamp_cmp(read.timestamp, timestamp) ? VK_SUCCESS : VK_TIMEOUT;
}

static VkResult
kgsl_poll_timestamp(struct tu_device *device, unsigned int context_id, unsigned int timestamp)
{
   if (kgsl_ts_xlate(context_id, &timestamp))
      return VK_SUCCESS;
   return kgsl_poll_timestamp_raw(device, context_id, timestamp);
}

/* safe_ioctl is not enough as restarted waits would not adjust the timeout
 * which could lead to waiting substantially longer than requested
 */
static VkResult
wait_timestamp_safe(struct tu_device *device, unsigned int context_id, unsigned int timestamp, uint64_t abs_timeout_ns)
{
   if (kgsl_ts_xlate(context_id, &timestamp))
      return VK_SUCCESS;
   struct kgsl_device_waittimestamp_ctxtid wait = {
      .context_id = context_id,
      .timestamp = timestamp,
   };

   while (true) {
      int timeout_ms = get_relative_ms(abs_timeout_ns);
      if (timeout_ms == 0)
         return kgsl_poll_timestamp_raw(device, context_id, timestamp);

      wait.timeout = timeout_ms;
      int ret = ioctl(device->fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &wait);
      if (ret == 0)
         return VK_SUCCESS;

      const int error = errno;
      if (error == EPROTO && kgsl_ignore_faults()) {
         mesa_logw("TU_HANGLOG: wait on ctx %u ts %u returned EPROTO -> ignored, retrying",
                   context_id, timestamp);
         continue;
      }
      if (error != EINTR && error != EAGAIN) {
         if (tu_hanglog_enabled && error != ETIMEDOUT)
            mesa_logw("TU_HANGLOG: wait on ctx %u ts %u failed: %s (errno %d), marker %u",
                      context_id, timestamp, strerror(error), error,
                      device->global_bo_map->hanglog_marker);
         if (tu_hanglog_enabled && error == EDEADLK) {
            static std::atomic<bool> once{false};
            if (!once.exchange(true))
               hanglog_query_faults(device, context_id);
         }
         return kgsl_timestamp_error(device, context_id, timestamp, "wait", error);
      }
   }
}

static VkResult
kgsl_queue_wait_fence(struct tu_queue *queue, uint32_t fence,
                      uint64_t timeout_ns)
{
   uint64_t abs_timeout_ns = os_time_get_absolute_timeout(timeout_ns);

   return wait_timestamp_safe(queue->device, queue->msm_queue_id, fence, abs_timeout_ns);
}

static VkResult
kgsl_syncobj_wait(struct tu_device *device,
                  struct kgsl_syncobj *s,
                  uint64_t abs_timeout_ns)
{
   if (s->state == KGSL_SYNCOBJ_STATE_UNSIGNALED) {
      /* If this syncobj is unsignaled we need to wait for it to resolve to a
       * valid syncobj prior to letting the rest of the wait continue, this
       * avoids needing kernel support for wait-before-signal semantics.
       */

      if (abs_timeout_ns == 0)
         return VK_TIMEOUT; // If this is a simple poll then we can return early

      pthread_mutex_lock(&device->submit_mutex);
      struct timespec abstime;
      timespec_from_nsec(&abstime, abs_timeout_ns);

      while (s->state == KGSL_SYNCOBJ_STATE_UNSIGNALED) {
         int ret;
         if (abs_timeout_ns == UINT64_MAX) {
            ret = pthread_cond_wait(&device->timeline_cond,
                                    &device->submit_mutex);
         } else {
            ret = pthread_cond_timedwait(&device->timeline_cond,
                                         &device->submit_mutex, &abstime);
         }
         if (ret != 0) {
            pthread_mutex_unlock(&device->submit_mutex);
            if (ret == ETIMEDOUT)
               return VK_TIMEOUT;

            return vk_errorf(device, VK_ERROR_UNKNOWN, "KGSL pending sync wait failed: %s", strerror(ret));
         }
      }

      pthread_mutex_unlock(&device->submit_mutex);
   }

   switch (s->state) {
   case KGSL_SYNCOBJ_STATE_SIGNALED:
      return VK_SUCCESS;

   case KGSL_SYNCOBJ_STATE_UNSIGNALED:
      return VK_TIMEOUT;

   case KGSL_SYNCOBJ_STATE_TS: {
      return wait_timestamp_safe(device, s->queue->msm_queue_id, s->timestamp, abs_timeout_ns);
   }

   case KGSL_SYNCOBJ_STATE_FD: {
      int ret = sync_wait(s->fd, get_relative_ms(abs_timeout_ns));
      if (ret) {
         const int error = errno;
         if (error == ETIME)
            return VK_TIMEOUT;

         return vk_errorf(device, VK_ERROR_UNKNOWN, "KGSL sync FD wait failed: fd %d, errno %d (%s)", s->fd, error,
                          strerror(error));
      } else {
         return VK_SUCCESS;
      }
   }

   default:
      UNREACHABLE("invalid syncobj state");
   }
}

#define kgsl_syncobj_foreach_state(syncobjs, filter)                                                                   \
   for (uint32_t i = 0; i < count && (sync = syncobjs[i]); i++)                                                        \
      if (sync->state == filter)

static VkResult
kgsl_syncobj_wait_any(struct tu_device* device, struct kgsl_syncobj **syncobjs, uint32_t count, uint64_t abs_timeout_ns)
{
   if (count == 0)
      return VK_TIMEOUT;
   else if (count == 1)
      return kgsl_syncobj_wait(device, syncobjs[0], abs_timeout_ns);

   uint32_t num_fds = 0;
   struct tu_queue *queue = NULL;
   struct kgsl_syncobj *sync = NULL;

   /* Simple case, we already have a signal one */
   kgsl_syncobj_foreach_state(syncobjs, KGSL_SYNCOBJ_STATE_SIGNALED)
      return VK_SUCCESS;

   kgsl_syncobj_foreach_state(syncobjs, KGSL_SYNCOBJ_STATE_FD)
      num_fds++;

   /* If we have TS from different queues we cannot compare them and would
    * have to convert them into FDs
    */
   bool convert_ts_to_fd = false;
   kgsl_syncobj_foreach_state(syncobjs, KGSL_SYNCOBJ_STATE_TS) {
      if (queue != NULL && sync->queue != queue) {
         convert_ts_to_fd = true;
         break;
      }
      queue = sync->queue;
   }

   /* If we have no FD nor TS syncobjs then we can return immediately */
   if (num_fds == 0 && queue == NULL)
      return VK_TIMEOUT;

   VkResult result = VK_TIMEOUT;

   struct u_vector poll_fds = { 0 };
   uint32_t lowest_timestamp = 0;

   if (convert_ts_to_fd || num_fds > 0)
      u_vector_init(&poll_fds, 4, sizeof(struct pollfd));

   uint32_t owned_fds = 0;

   if (convert_ts_to_fd) {
      kgsl_syncobj_foreach_state(syncobjs, KGSL_SYNCOBJ_STATE_TS) {
         struct pollfd *poll_fd = static_cast<struct pollfd *>(u_vector_add(&poll_fds));
         poll_fd->fd = timestamp_to_fd(sync->queue, sync->timestamp);
         if (poll_fd->fd < 0) {
            result = kgsl_sync_error(device, "timestamp fence creation", errno);
            goto finish;
         }
         owned_fds++;
         poll_fd->events = POLLIN;
      }
   } else {
      /* TSs could be merged by finding the one with the lowest timestamp */
      bool first_ts = true;
      kgsl_syncobj_foreach_state(syncobjs, KGSL_SYNCOBJ_STATE_TS) {
         if (first_ts || timestamp_cmp(lowest_timestamp, sync->timestamp)) {
            first_ts = false;
            lowest_timestamp = sync->timestamp;
         }
      }

      if (num_fds && queue) {
         struct pollfd *poll_fd = static_cast<struct pollfd *>(u_vector_add(&poll_fds));
         poll_fd->fd = timestamp_to_fd(queue, lowest_timestamp);
         if (poll_fd->fd < 0) {
            result = kgsl_sync_error(device, "timestamp fence creation", errno);
            goto finish;
         }
         owned_fds++;
         poll_fd->events = POLLIN;
      }
   }

   if (num_fds) {
      kgsl_syncobj_foreach_state(syncobjs, KGSL_SYNCOBJ_STATE_FD) {
         struct pollfd *poll_fd = static_cast<struct pollfd *>(u_vector_add(&poll_fds));
         poll_fd->fd = sync->fd;
         poll_fd->events = POLLIN;
      }
   }

   if (u_vector_length(&poll_fds) == 0) {
      result = wait_timestamp_safe(device, queue->msm_queue_id, lowest_timestamp, MIN2(abs_timeout_ns, INT64_MAX));
   } else {
      int ret, i;

      struct pollfd *fds = static_cast<struct pollfd *>(poll_fds.data);
      uint32_t fds_count = u_vector_length(&poll_fds);
      do {
         ret = poll(fds, fds_count, get_relative_ms(abs_timeout_ns));
         if (ret > 0) {
            for (i = 0; i < fds_count; i++) {
               if (fds[i].revents & (POLLERR | POLLNVAL)) {
                  errno = EINVAL;
                  ret = -1;
                  break;
               }
            }
            break;
         } else if (ret == 0) {
            errno = ETIME;
            break;
         }
      } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

      const int error = errno;
      if (ret > 0) {
         result = VK_SUCCESS;
      } else if (ret == 0) {
         result = VK_TIMEOUT;
      } else {
         result =
            vk_errorf(device, VK_ERROR_UNKNOWN, "KGSL sync FD poll failed: errno %d (%s)", error, strerror(error));
      }
   }

finish:
   for (uint32_t i = 0; i < owned_fds; i++)
      close(((struct pollfd *) poll_fds.data)[i].fd);
   u_vector_finish(&poll_fds);
   return result;
}

static VkResult
kgsl_syncobj_export(struct tu_device *device, struct kgsl_syncobj *s, int *pFd)
{
   if (!pFd)
      return VK_SUCCESS;

   switch (s->state) {
   case KGSL_SYNCOBJ_STATE_SIGNALED:
   case KGSL_SYNCOBJ_STATE_UNSIGNALED:
      /* Getting a sync FD from an unsignaled syncobj is UB in Vulkan */
      *pFd = -1;
      return VK_SUCCESS;

   case KGSL_SYNCOBJ_STATE_FD: {
      int fd = os_dupfd_cloexec(s->fd);
      if (fd < 0)
         return kgsl_sync_error(device, "sync FD export", errno);
      *pFd = fd;
      return VK_SUCCESS;
   }

   case KGSL_SYNCOBJ_STATE_TS: {
      int fd = kgsl_syncobj_ts_to_fd(s);
      if (fd < 0)
         return kgsl_sync_error(device, "timestamp fence export", errno);
      *pFd = fd;
      return VK_SUCCESS;
   }

   default:
      UNREACHABLE("Invalid syncobj state");
   }
}

static VkResult
kgsl_syncobj_import(struct kgsl_syncobj *s, int fd)
{
   kgsl_syncobj_reset(s);
   if (fd >= 0) {
      s->state = KGSL_SYNCOBJ_STATE_FD;
      s->fd = fd;
   } else {
      s->state = KGSL_SYNCOBJ_STATE_SIGNALED;
   }

   return VK_SUCCESS;
}

/* Merges multiple kgsl_syncobjs into a single one which is only signalled
 * after all submitted syncobjs are signalled
 */
static VkResult
kgsl_syncobj_merge(struct tu_device *device,
                   const struct kgsl_syncobj **syncobjs,
                   uint32_t count,
                   struct kgsl_syncobj *out)
{
   struct kgsl_syncobj ret;
   kgsl_syncobj_init(&ret, true);
   VkResult result = VK_SUCCESS;

   for (uint32_t i = 0; i < count; ++i) {
      const struct kgsl_syncobj *sync = syncobjs[i];
      if (sync->state == KGSL_SYNCOBJ_STATE_SIGNALED)
         continue;
      if (sync->state == KGSL_SYNCOBJ_STATE_UNSIGNALED) {
         kgsl_syncobj_reset(&ret);
         *out = ret;
         return VK_SUCCESS;
      }

      if (ret.state == KGSL_SYNCOBJ_STATE_SIGNALED) {
         /* If `ret` wasn't assigned before - just dup the `sync` into it. */
         result = kgsl_syncobj_dup(device, sync, &ret);
         if (result != VK_SUCCESS)
            goto fail;
         continue;
      }

      if (ret.state == KGSL_SYNCOBJ_STATE_TS && sync->state == KGSL_SYNCOBJ_STATE_TS && ret.queue == sync->queue) {
         /* On the same queue we can pick the highest TS, since we wait for all. */
         ret.timestamp = max_ts(ret.timestamp, sync->timestamp);
         continue;
      }

      if (ret.state == KGSL_SYNCOBJ_STATE_TS) {
         /* Queues don't match or sync is an FD - convert aggregate `ret` to FD. */
         int fd = kgsl_syncobj_ts_to_fd(&ret);
         if (fd < 0) {
            result = kgsl_sync_error(device, "timestamp fence creation", errno);
            goto fail;
         }
         ret.fd = fd;
         ret.state = KGSL_SYNCOBJ_STATE_FD;
      }

      int sync_fd = sync->fd;
      if (sync->state == KGSL_SYNCOBJ_STATE_TS) {
         sync_fd = kgsl_syncobj_ts_to_fd(sync);
         if (sync_fd < 0) {
            result = kgsl_sync_error(device, "timestamp fence creation", errno);
            goto fail;
         }
      }

      int fd = sync_merge("tu_sync", ret.fd, sync_fd);
      const int error = errno;
      if (sync->state == KGSL_SYNCOBJ_STATE_TS)
         close(sync_fd);
      if (fd < 0) {
         result = kgsl_sync_error(device, "sync FD merge", error);
         goto fail;
      }
      close(ret.fd);
      ret.fd = fd;
   }

   *out = ret;
   return VK_SUCCESS;

fail:
   kgsl_syncobj_destroy(&ret);
   return result;
}

struct vk_kgsl_syncobj
{
   struct vk_sync vk;
   struct kgsl_syncobj syncobj;
};

static VkResult
vk_kgsl_sync_init(struct vk_device *device,
                  struct vk_sync *sync,
                  uint64_t initial_value)
{
   struct vk_kgsl_syncobj *s = container_of(sync, struct vk_kgsl_syncobj, vk);
   kgsl_syncobj_init(&s->syncobj, initial_value != 0);
   return VK_SUCCESS;
}

static void
vk_kgsl_sync_finish(struct vk_device *device, struct vk_sync *sync)
{
   struct vk_kgsl_syncobj *s = container_of(sync, struct vk_kgsl_syncobj, vk);
   kgsl_syncobj_destroy(&s->syncobj);
}

static VkResult
vk_kgsl_sync_reset(struct vk_device *device, struct vk_sync *sync)
{
   struct vk_kgsl_syncobj *s = container_of(sync, struct vk_kgsl_syncobj, vk);
   kgsl_syncobj_reset(&s->syncobj);
   return VK_SUCCESS;
}

static VkResult
vk_kgsl_sync_move(struct vk_device *device,
                  struct vk_sync *dst,
                  struct vk_sync *src)
{
   struct vk_kgsl_syncobj *d = container_of(dst, struct vk_kgsl_syncobj, vk);
   struct vk_kgsl_syncobj *s = container_of(src, struct vk_kgsl_syncobj, vk);
   kgsl_syncobj_reset(&d->syncobj);
   d->syncobj = s->syncobj;
   kgsl_syncobj_init(&s->syncobj, false);
   return VK_SUCCESS;
}

static VkResult
vk_kgsl_sync_wait(struct vk_device *_device,
                  struct vk_sync *sync,
                  uint64_t wait_value,
                  enum vk_sync_wait_flags wait_flags,
                  uint64_t abs_timeout_ns)
{
   struct tu_device *device = container_of(_device, struct tu_device, vk);
   struct vk_kgsl_syncobj *s = container_of(sync, struct vk_kgsl_syncobj, vk);

   if (wait_flags & VK_SYNC_WAIT_PENDING)
      return VK_SUCCESS;

   return kgsl_syncobj_wait(device, &s->syncobj, abs_timeout_ns);
}

static VkResult
vk_kgsl_sync_wait_many(struct vk_device *_device,
                       uint32_t wait_count,
                       const struct vk_sync_wait *waits,
                       enum vk_sync_wait_flags wait_flags,
                       uint64_t abs_timeout_ns)
{
   struct tu_device *device = container_of(_device, struct tu_device, vk);

   if (wait_flags & VK_SYNC_WAIT_PENDING)
      return VK_SUCCESS;

   if (wait_flags & VK_SYNC_WAIT_ANY) {
      struct kgsl_syncobj *syncobjs[wait_count];
      for (uint32_t i = 0; i < wait_count; i++) {
         syncobjs[i] =
            &container_of(waits[i].sync, struct vk_kgsl_syncobj, vk)->syncobj;
      }

      return kgsl_syncobj_wait_any(device, syncobjs, wait_count,
                                   abs_timeout_ns);
   } else {
      for (uint32_t i = 0; i < wait_count; i++) {
         struct vk_kgsl_syncobj *s =
            container_of(waits[i].sync, struct vk_kgsl_syncobj, vk);

         VkResult result =
            kgsl_syncobj_wait(device, &s->syncobj, abs_timeout_ns);
         if (result != VK_SUCCESS)
            return result;
      }
      return VK_SUCCESS;
   }
}

static VkResult
vk_kgsl_sync_import_sync_file(struct vk_device *device,
                              struct vk_sync *sync,
                              int fd)
{
   struct vk_kgsl_syncobj *s = container_of(sync, struct vk_kgsl_syncobj, vk);
   if (fd >= 0) {
      fd = os_dupfd_cloexec(fd);
      if (fd < 0) {
         mesa_loge("vk_kgsl_sync_import_sync_file: dup failed: %s",
                   strerror(errno));
         return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   }
   return kgsl_syncobj_import(&s->syncobj, fd);
}

static VkResult
vk_kgsl_sync_export_sync_file(struct vk_device *device,
                              struct vk_sync *sync,
                              int *pFd)
{
   struct vk_kgsl_syncobj *s = container_of(sync, struct vk_kgsl_syncobj, vk);
   return kgsl_syncobj_export(container_of(device, struct tu_device, vk), &s->syncobj, pFd);
}

const struct vk_sync_type vk_kgsl_sync_type = {
   .size = sizeof(struct vk_kgsl_syncobj),
   .features = static_cast<enum vk_sync_features>(
               VK_SYNC_FEATURE_BINARY |
                VK_SYNC_FEATURE_GPU_WAIT |
                VK_SYNC_FEATURE_GPU_MULTI_WAIT |
                VK_SYNC_FEATURE_CPU_WAIT |
                VK_SYNC_FEATURE_CPU_RESET |
                VK_SYNC_FEATURE_WAIT_ANY |
                VK_SYNC_FEATURE_WAIT_PENDING),
   .init = vk_kgsl_sync_init,
   .finish = vk_kgsl_sync_finish,
   .reset = vk_kgsl_sync_reset,
   .move = vk_kgsl_sync_move,
   .wait = vk_kgsl_sync_wait,
   .wait_many = vk_kgsl_sync_wait_many,
   .import_sync_file = vk_kgsl_sync_import_sync_file,
   .export_sync_file = vk_kgsl_sync_export_sync_file,
};

struct tu_kgsl_queue_submit {
   struct util_dynarray commands;
   struct util_dynarray ranges;
   struct util_dynarray bind_cmds;
   struct tu_sparse_vma *cur_vma;
   unsigned cur_vma_range_start;
};

static void *
kgsl_submit_create(struct tu_device *device)
{
   return vk_zalloc(&device->vk.alloc, sizeof(struct tu_kgsl_queue_submit), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
}

static void
kgsl_submit_finish(struct tu_device *device,
                   void *_submit)
{
   struct tu_kgsl_queue_submit *submit =
      static_cast<struct tu_kgsl_queue_submit *>(_submit);

   util_dynarray_fini(&submit->commands);
   util_dynarray_fini(&submit->ranges);
   util_dynarray_fini(&submit->bind_cmds);
   vk_free(&device->vk.alloc, submit);
}

static void
kgsl_submit_add_entries(struct tu_device *device, void *_submit,
                        struct tu_cs_entry *entries, unsigned num_entries)
{
   struct tu_kgsl_queue_submit *submit =
      static_cast<struct tu_kgsl_queue_submit *>(_submit);

   struct kgsl_command_object *cmds = static_cast<struct kgsl_command_object *>(
      util_dynarray_grow(&submit->commands, struct kgsl_command_object,
                      num_entries));

   for (unsigned i = 0; i < num_entries; i++) {
      cmds[i] = (struct kgsl_command_object) {
         .gpuaddr = entries[i].bo->iova + entries[i].offset,
         .size = entries[i].size,
         .flags = KGSL_CMDLIST_IB,
         .id = entries[i].bo->gem_handle,
      };
   }
}

static void
kgsl_submit_add_bind(struct tu_device *device,
                     void *_submit,
                     struct tu_sparse_vma *vma, uint64_t vma_offset,
                     struct tu_bo *bo, uint64_t bo_offset,
                     uint64_t size)
{
   struct tu_kgsl_queue_submit *submit =
      static_cast<struct tu_kgsl_queue_submit *>(_submit);

   if (vma != submit->cur_vma) {
      unsigned range_count =
         util_dynarray_num_elements(&submit->ranges,
                                    struct kgsl_gpumem_bind_range);
      if (submit->cur_vma) {
         struct kgsl_gpu_aux_command_bind *last_bind =
            util_dynarray_top_ptr(&submit->bind_cmds,
                                  struct kgsl_gpu_aux_command_bind);
         last_bind->numranges = range_count - submit->cur_vma_range_start;
      }

      struct kgsl_gpu_aux_command_bind bind = {
         .rangeslist = submit->ranges.size,
         .numranges = 0,
         .rangesize = sizeof(struct kgsl_gpumem_bind_range),
         .target = vma->kgsl.virtual_bo->gem_handle,
      };


      util_dynarray_append(&submit->bind_cmds, bind);

      submit->cur_vma = vma;
      submit->cur_vma_range_start = range_count;
   }

   struct kgsl_gpumem_bind_range range = {
      .child_offset = bo_offset,
      .target_offset = vma_offset,
      .length = size,
      .child_id = bo ? bo->gem_handle : 0,
      .op = bo ? KGSL_GPUMEM_RANGE_OP_BIND : KGSL_GPUMEM_RANGE_OP_UNBIND,
   };

   util_dynarray_append(&submit->ranges, range);
}

/* We don't know the actual CPU pointers until we've finished adding all the
 * bind commands, so we put the offset from the base instead. We need to write
 * the actual pointer after all the ranges are added. We also need to fill out
 * of the size of the last command.
 */
static void
kgsl_bind_finalize(struct tu_kgsl_queue_submit *submit)
{
   unsigned range_count =
      util_dynarray_num_elements(&submit->ranges,
                                 struct kgsl_gpumem_bind_range);
   struct kgsl_gpu_aux_command_bind *last_bind =
      util_dynarray_top_ptr(&submit->bind_cmds,
                            struct kgsl_gpu_aux_command_bind);
   last_bind->numranges = range_count - submit->cur_vma_range_start;

   util_dynarray_foreach (&submit->bind_cmds,
                          struct kgsl_gpu_aux_command_bind, bind) {
      bind->rangeslist += static_cast<uint64_t>(reinterpret_cast<uintptr_t>(submit->ranges.data));
   }
}


/* TU_SAFEMODE: see tu_util.h. Checked before every submission. */
static void
kgsl_safemode_before_submit(struct tu_queue *queue)
{
   static int ms = -1;
   if (unlikely(ms < 0)) {
      const char *v = os_get_option("TU_SAFEMODE");
      if (!v || !*v || !strcmp(v, "0") || !strcmp(v, "false"))
         ms = 0;
      else if (!strcmp(v, "1") || !strcmp(v, "true"))
         ms = 3000;
      else
         ms = MAX2(atoi(v), 0);
   }
   if (!ms)
      return;

   struct tu_device *dev = queue->device;
   struct kgsl_context_property_fault f = {};
   struct kgsl_context_property cp = {
      .data = (uint64_t)(uintptr_t)&f,
      .size = sizeof(f),
      .type = KGSL_CONTEXT_PROP_FAULTS,
      .contextid = queue->msm_queue_id,
   };
   if (get_kgsl_prop(dev->fd, KGSL_PROP_CONTEXT_PROPERTY, &cp, sizeof(cp)) == VK_SUCCESS &&
       (uint32_t)f.faults > queue->safemode_faults) {
      queue->safemode_faults = f.faults;
      /* Escalate: a fault during safe mode or soon after it ended doubles
       * the window (up to 60 s); a fault after a quiet period resets it.
       */
      static int64_t cur_ms = 0;
      const int64_t now = os_time_get_nano();
      const int64_t until = tu_safe_until_ns.load(std::memory_order_relaxed);
      if (cur_ms && until && now < until + 5000ll * 1000000ll)
         cur_ms = MIN2(cur_ms * 2, 60000);
      else
         cur_ms = ms;
      tu_safe_level.store(cur_ms >= 4ll * ms ? 2 : 1, std::memory_order_relaxed);
      tu_safe_until_ns.store(now + cur_ms * 1000000ll, std::memory_order_relaxed);
      mesa_logw("TU_SAFEMODE: kernel GPU fault #%u on ctx %u -> safe mode level %d for %lld ms",
                (unsigned)f.faults, queue->msm_queue_id,
                tu_safe_level.load(std::memory_order_relaxed), (long long)cur_ms);
   }

   /* In safe mode, keep at most one submission in flight on this queue. */
   if (tu_safe_active() && queue->fence >= 0) {
      wait_timestamp_safe(dev, queue->msm_queue_id, queue->fence,
                          os_time_get_absolute_timeout(200 * 1000000ull));
   }
}

static VkResult
kgsl_queue_submit(struct tu_queue *queue, void *_submit,
                  struct vk_sync_wait *waits, uint32_t wait_count,
                  struct vk_sync_signal *signals, uint32_t signal_count,
                  struct tu_u_trace_submission_data *u_trace_submission_data)
{
   struct tu_kgsl_queue_submit *submit =
      static_cast<struct tu_kgsl_queue_submit *>(_submit);

#if HAVE_PERFETTO
   uint64_t start_ts = tu_perfetto_begin_submit();
#endif

   kgsl_safemode_before_submit(queue);

   if (submit->commands.size == 0 && submit->bind_cmds.size == 0) {
      /* This handles the case where we have a wait and no commands to submit.
       * It is necessary to handle this case separately as the kernel will not
       * advance the GPU timeline if a submit with no commands is made, even
       * though it will return an incremented fence timestamp (which will
       * never be signaled).
       */
      const struct kgsl_syncobj *wait_semaphores[wait_count + 1];
      for (uint32_t i = 0; i < wait_count; i++) {
         wait_semaphores[i] = &container_of(waits[i].sync,
                                            struct vk_kgsl_syncobj, vk)
                                  ->syncobj;
      }

      struct kgsl_syncobj last_submit_sync;
      if (queue->fence >= 0)
         last_submit_sync = (struct kgsl_syncobj) {
            .state = KGSL_SYNCOBJ_STATE_TS,
            .queue = queue,
            .timestamp = queue->fence,
         };
      else
         last_submit_sync = (struct kgsl_syncobj) {
            .state = KGSL_SYNCOBJ_STATE_SIGNALED,
         };

      wait_semaphores[wait_count] = &last_submit_sync;

      struct kgsl_syncobj wait_sync;
      VkResult result = kgsl_syncobj_merge(queue->device, wait_semaphores, wait_count + 1, &wait_sync);
      if (result != VK_SUCCESS)
         return result;
      assert(wait_sync.state !=
             KGSL_SYNCOBJ_STATE_UNSIGNALED); // Would wait forever

      if (signal_count == 1) {
         /* Move instead of duplicating the syncobj, as we don't need to
          * keep the original one around.
          */
         struct kgsl_syncobj *signal_sync =
            &container_of(signals[0].sync, struct vk_kgsl_syncobj, vk)
                ->syncobj;

         kgsl_syncobj_reset(signal_sync);
         *signal_sync = wait_sync;
      } else {
         struct kgsl_syncobj copies[signal_count];
         for (uint32_t i = 0; i < signal_count; i++) {
            result = kgsl_syncobj_dup(queue->device, &wait_sync, &copies[i]);
            if (result != VK_SUCCESS) {
               for (uint32_t j = 0; j < i; j++)
                  kgsl_syncobj_destroy(&copies[j]);
               kgsl_syncobj_destroy(&wait_sync);
               return result;
            }
         }
         for (uint32_t i = 0; i < signal_count; i++) {
            struct kgsl_syncobj *signal_sync =
               &container_of(signals[i].sync, struct vk_kgsl_syncobj, vk)
                   ->syncobj;
            kgsl_syncobj_reset(signal_sync);
            *signal_sync = copies[i];
         }
         kgsl_syncobj_destroy(&wait_sync);
      }

      kgsl_syncobj_destroy(&last_submit_sync);

      return VK_SUCCESS;
   }

   VkResult result = VK_SUCCESS;
   bool profiling_submitted = false;

   if (submit->bind_cmds.size != 0)
      kgsl_bind_finalize(submit);

   const struct kgsl_syncobj *wait_semaphores[wait_count];
   for (uint32_t i = 0; i < wait_count; i++) {
      wait_semaphores[i] = &container_of(waits[i].sync, struct vk_kgsl_syncobj, vk)->syncobj;
   }

   struct kgsl_syncobj wait_sync;
   result = kgsl_syncobj_merge(queue->device, wait_semaphores, wait_count, &wait_sync);
   if (result != VK_SUCCESS)
      return result;
   assert(wait_sync.state != KGSL_SYNCOBJ_STATE_UNSIGNALED); // Would wait forever

   if (u_trace_submission_data) {
      mtx_lock(&queue->device->kgsl_profiling_mutex);
      result =
         tu_suballoc_bo_alloc(&u_trace_submission_data->kgsl_timestamp_bo, &queue->device->kgsl_profiling_suballoc,
                              sizeof(struct kgsl_cmdbatch_profiling_buffer), 4);
      mtx_unlock(&queue->device->kgsl_profiling_mutex);
      if (result != VK_SUCCESS) {
         kgsl_syncobj_destroy(&wait_sync);
         return result;
      }
   }

   struct kgsl_command_object profile_obj = { 0 };

   struct kgsl_cmdbatch_profiling_buffer *profiling_buffer = NULL;
   if (u_trace_submission_data) {
      struct tu_suballoc_bo *bo = &u_trace_submission_data->kgsl_timestamp_bo;

      profile_obj = (struct kgsl_command_object) {
         .offset = bo->iova - bo->bo->iova,
         .gpuaddr = bo->bo->iova,
         .size = sizeof(struct kgsl_cmdbatch_profiling_buffer),
         .flags = KGSL_OBJLIST_MEMOBJ | KGSL_OBJLIST_PROFILE,
         .id = bo->bo->gem_handle,
      };
      profiling_buffer =
         static_cast<struct kgsl_cmdbatch_profiling_buffer *>(tu_suballoc_bo_map(bo));
      memset(profiling_buffer, 0, sizeof(*profiling_buffer));
   }

   struct kgsl_cmd_syncpoint_timestamp ts;
   struct kgsl_cmd_syncpoint_fence fn;
   struct kgsl_command_syncpoint sync = { 0 };
   bool has_sync = false;
   switch (wait_sync.state) {
   case KGSL_SYNCOBJ_STATE_SIGNALED:
      break;

   case KGSL_SYNCOBJ_STATE_TS:
      ts.context_id = wait_sync.queue->msm_queue_id;
      ts.timestamp = wait_sync.timestamp;
      if (kgsl_ts_xlate(ts.context_id, &ts.timestamp))
         break; /* belongs to a dead context: nothing to wait for */

      has_sync = true;
      sync.type = KGSL_CMD_SYNCPOINT_TYPE_TIMESTAMP;
      sync.priv = reinterpret_cast<uintptr_t>(&ts);
      sync.size = sizeof(ts);
      break;

   case KGSL_SYNCOBJ_STATE_FD:
      fn.fd = wait_sync.fd;

      has_sync = true;
      sync.type = KGSL_CMD_SYNCPOINT_TYPE_FENCE;
      sync.priv = reinterpret_cast<uintptr_t>(&fn);
      sync.size = sizeof(fn);
      break;

   default:
      UNREACHABLE("invalid syncobj state");
   }

   int ret;
   uint32_t timestamp = 0;
   uint64_t gpu_offset = 0;

   if (submit->bind_cmds.size == 0) {
      struct kgsl_gpu_command req = {
         .flags = KGSL_CMDBATCH_SUBMIT_IB_LIST,
         .cmdlist = reinterpret_cast<uintptr_t>(submit->commands.data),
         .cmdsize = sizeof(struct kgsl_command_object),
         .numcmds = util_dynarray_num_elements(&submit->commands,
                                               struct kgsl_command_object),
         .synclist = reinterpret_cast<uintptr_t>(&sync),
         .syncsize = sizeof(sync),
         .numsyncs = has_sync != 0 ? 1 : 0,
         .context_id = queue->msm_queue_id,
      };

      if (kgsl_pwr_level() >= 0)
         req.flags |= KGSL_CMDBATCH_PWR_CONSTRAINT;

      if (profiling_buffer) {
         req.flags |= KGSL_CMDBATCH_PROFILING;
         req.objlist = reinterpret_cast<uintptr_t>(&profile_obj);
         req.objsize = sizeof(profile_obj);
         req.numobjs = 1;
      }

      ret = safe_ioctl(queue->device->physical_device->local_fd,
                       IOCTL_KGSL_GPU_COMMAND, &req);

      const int error = errno;
      /* EPROTO - Device faulted since the last check. */
      profiling_submitted = profiling_buffer && (ret == 0 || error == EPROTO);
      if (ret && (tu_hanglog_enabled || kgsl_ignore_faults()))
         mesa_logw("TU_HANGLOG: submit on ctx %u returned %s (errno %d), ts %u, marker %u%s",
                   queue->msm_queue_id, strerror(error), error, req.timestamp,
                   queue->device->global_bo_map->hanglog_marker,
                   (error == EPROTO && kgsl_ignore_faults()) ? " -> ignored" : "");
      if (ret && error == EPROTO && kgsl_ignore_faults())
         ret = 0;
      if (ret && kgsl_ctx_recover_enabled() &&
          (error == EDEADLK || error == ENOENT || error == EINVAL) &&
          kgsl_queue_recover(queue) == 0) {
         req.context_id = queue->msm_queue_id;
         req.numsyncs = 0; /* previous work is gone, do not wait on it */
         ret = safe_ioctl(queue->device->physical_device->local_fd,
                          IOCTL_KGSL_GPU_COMMAND, &req);
         if (ret && errno == EPROTO)
            ret = 0;
      }
      if (ret) {
         kgsl_syncobj_destroy(&wait_sync);
         result = vk_device_set_lost(&queue->device->vk, "submit failed: %s\n", strerror(error));
         goto fail_submit;
      }
      timestamp = req.timestamp;
      if (unlikely(tu_hanglog_enabled)) {
         struct tu_hanglog_submit *hs =
            &tu_hanglog_submits[timestamp & (TU_HANGLOG_SUBMIT_RING - 1)];
         *hs = tu_hanglog_pending;
         hs->ts = timestamp;
         hs->ctx = queue->msm_queue_id;
         hs->t_ns = os_time_get_nano();
         tu_hanglog_pending.n = 0;
         tu_hanglog_pending.ncmds = 0;
         queue->hanglog_last_ts.store(timestamp, std::memory_order_relaxed);
         queue->hanglog_has_submit.store(true, std::memory_order_release);
      }
   } else {
      /* kgsl doesn't support multiple bind commands at once */
      uint32_t i = 0;
      util_dynarray_foreach(&submit->bind_cmds,
                            struct kgsl_gpu_aux_command_bind, bind) {
         bool do_sync = has_sync && i == 0;

         struct kgsl_gpu_aux_command_generic aux = {
            .priv = reinterpret_cast<uintptr_t>(bind),
            .size = sizeof(*bind),
            .type = KGSL_GPU_AUX_COMMAND_BIND,
         };

         uint32_t flags = KGSL_GPU_AUX_COMMAND_BIND;
         if (do_sync)
            flags |= KGSL_GPU_AUX_COMMAND_SYNC;

         struct kgsl_gpu_aux_command req = {
            .flags = flags,
            .cmdlist = reinterpret_cast<uintptr_t>(&aux),
            .cmdsize = sizeof(aux),
            .numcmds = 1,
            .synclist = reinterpret_cast<uintptr_t>(&sync),
            .syncsize = sizeof(sync),
            .numsyncs = do_sync ? 1 : 0,
            .context_id = queue->msm_queue_id,
         };
         ret = safe_ioctl(queue->device->physical_device->local_fd,
                          IOCTL_KGSL_GPU_AUX_COMMAND, &req);

         if (ret) {
            const int error = errno;
            kgsl_syncobj_destroy(&wait_sync);
            result = vk_device_set_lost(&queue->device->vk, "bind submit failed: %s\n", strerror(error));
            goto fail_submit;
         }

         timestamp = req.timestamp;
         i++;
      }
   }

#if HAVE_PERFETTO
   if (profiling_buffer) {
      struct tu_perfetto_clocks clocks = { 0 };
      /* We need to wait for KGSL to queue the GPU command before we can read
       * the timestamp. Since this is just for profiling and doesn't take too
       * long, we can just busy-wait for it.
       */
      while (p_atomic_read(&profiling_buffer->gpu_ticks_queued) == 0);

      struct kgsl_perfcounter_read_group perf = {
         .groupid = KGSL_PERFCOUNTER_GROUP_ALWAYSON,
         .countable = 0,
         .value = 0
      };
      struct kgsl_perfcounter_read req = {
         .reads = &perf,
         .count = 1,
      };
      if (safe_ioctl(queue->device->fd, IOCTL_KGSL_PERFCOUNTER_READ, &req) == 0) {
         /* Older KGSL has some kind of garbage in upper 32 bits */
         uint64_t offseted_gpu_ts = perf.value & 0xffffffff;
         gpu_offset = tu_device_ticks_to_ns(queue->device, offseted_gpu_ts - profiling_buffer->gpu_ticks_queued);
         clocks.cpu = profiling_buffer->wall_clock_ns;
         clocks.gpu_ts = tu_device_ticks_to_ns(queue->device, profiling_buffer->gpu_ticks_queued);
         clocks.gpu_ts_offset = gpu_offset;
      }
      clocks = tu_perfetto_end_submit(queue, queue->device->submit_count,
                                      start_ts, &clocks);
      gpu_offset = clocks.gpu_ts_offset;
   }
#endif

   kgsl_syncobj_destroy(&wait_sync);

   timestamp += kgsl_ctx_offset(queue->msm_queue_id);
   p_atomic_set(&queue->fence, timestamp);

   for (uint32_t i = 0; i < signal_count; i++) {
      struct kgsl_syncobj *signal_sync =
         &container_of(signals[i].sync, struct vk_kgsl_syncobj, vk)
             ->syncobj;

      kgsl_syncobj_reset(signal_sync);
      signal_sync->state = KGSL_SYNCOBJ_STATE_TS;
      signal_sync->queue = queue;
      signal_sync->timestamp = timestamp;
   }

   if (u_trace_submission_data) {
      struct tu_u_trace_submission_data *submission_data =
         u_trace_submission_data;
      submission_data->gpu_ts_offset = gpu_offset;
   }

fail_submit:
   if (result != VK_SUCCESS && u_trace_submission_data && !profiling_submitted) {
      mtx_lock(&queue->device->kgsl_profiling_mutex);
      tu_suballoc_bo_free(&queue->device->kgsl_profiling_suballoc,
                          &u_trace_submission_data->kgsl_timestamp_bo);
      u_trace_submission_data->kgsl_timestamp_bo = {};
      mtx_unlock(&queue->device->kgsl_profiling_mutex);
   }

   return result;
}

static VkResult
kgsl_device_init(struct tu_device *dev)
{
   dev->fd = dev->physical_device->local_fd;
   return VK_SUCCESS;
}

static void
kgsl_device_finish(struct tu_device *dev)
{
   /* No-op */
}

static int
kgsl_device_get_gpu_timestamp(struct tu_device *dev, uint64_t *ts)
{
   UNREACHABLE("");
   return 0;
}

static int
kgsl_device_get_suspend_count(struct tu_device *dev, uint64_t *suspend_count)
{
   /* kgsl doesn't have a way to get it */
   *suspend_count = 0;
   return 0;
}


/* ---------------- TU_HANGLOG watchdog ---------------- */

static const char *
hanglog_kind_str(uint8_t k)
{
   switch (k) {
   case 1: return "draw";
   case 2: return "draw-indirect";
   case 3: return "dispatch";
   case 4: return "dispatch-indirect";
   case 5: return "CCU->SYSMEM";
   case 6: return "CCU->GMEM";
   case 7: return "RENDER-START(SYSMEM)";
   case 8: return "RENDER-START(GMEM)";
   case 9: return "SYSMEM-CLEAR(2D)";
   case 10: return "SYSMEM-CLEAR(3D)";
   case 11: return "BEGIN-CMDBUF";
   case 12: return "END-CMDBUF";
   case 13: return "PipelineBarrier";
   case 14: return "WaitEvents";
   case 15: return "SetEvent";
   case 16: return "ResetEvent";
   case 17: return "CopyBuffer";
   case 18: return "CopyImage";
   case 19: return "CopyBufferToImage";
   case 20: return "CopyImageToBuffer";
   case 21: return "BlitImage";
   case 22: return "ResolveImage";
   case 23: return "ClearColorImage";
   case 24: return "ClearDepthStencilImage";
   case 25: return "FillBuffer";
   case 26: return "UpdateBuffer";
   case 27: return "ResetQueryPool";
   case 28: return "BeginQuery";
   case 29: return "EndQuery";
   case 30: return "WriteTimestamp";
   case 31: return "CopyQueryPoolResults";
   case 32: return "ExecuteCommands";
   case 33: return "BeginRendering";
   case 34: return "WriteBufferMarker";
   case 35: return "ClearAttachments";
   default: return "?";
   }
}

static void
hanglog_print_entry(const char *tag, uint32_t id)
{
   const struct tu_hanglog_entry *e = &tu_hanglog_ring[id & (TU_HANGLOG_RING - 1)];
   if (e->id != id) {
      mesa_logw("TU_HANGLOG:   %s id %u: (not in ring)", tag, id);
      return;
   }
   const struct tu_hanglog_rpmode *m =
      &tu_hanglog_rpmodes[tu_hanglog_rp_slot(e->cmd_seq, e->rp)];
   if (e->kind >= 11) {
      const char *rpmode = (m->cmd_seq == e->cmd_seq && m->rp == e->rp) ?
         (m->mode == 1 ? "SYSMEM" : "GMEM") : "?";
      if (e->qpool)
         mesa_logw("TU_HANGLOG:   %s id %u: %s cmdbuf#%u rp%u idx%u %s%s pool %p type %u q %u+%u flags 0x%x",
                   tag, id, hanglog_kind_str(e->kind), e->cmd_seq, e->rp, e->idx,
                   e->count ? "INSIDE-RP:" : "outside-rp", e->count ? rpmode : "",
                   e->qpool, e->qtype, e->qfirst, e->qcount, e->qflags);
      else
         mesa_logw("TU_HANGLOG:   %s id %u: %s cmdbuf#%u rp%u idx%u %s%s",
                   tag, id, hanglog_kind_str(e->kind), e->cmd_seq, e->rp, e->idx,
                   e->count ? "INSIDE-RP:" : "outside-rp", e->count ? rpmode : "");
      return;
   }
   const char *mode = (e->kind >= 3) ? "-" :
      (m->cmd_seq == e->cmd_seq && m->rp == e->rp) ?
         (m->mode == 1 ? "SYSMEM" : "GMEM") : "?";
   mesa_logw("TU_HANGLOG:   %s id %u: %s cmdbuf#%u rp%u(%s) idx%u uav=0x%x vs=%s fs=%s cs=%s "
             "count=%u inst=0x%x area=%ux%u colors=%u fmt=%u depth=%u samples=%u",
             tag, id, hanglog_kind_str(e->kind), e->cmd_seq, e->rp, mode, e->idx,
             e->uav_mask, e->vs, e->fs, e->cs, e->count, e->instances,
             e->w, e->h, e->ncolor, e->color_fmt, e->depth_fmt, e->samples);
   if (e->kind == 7 || e->kind == 8)
      mesa_logw("TU_HANGLOG:   %s id %u:   pass feat 0x%x%s", tag, id,
                e->qflags & 0x7fffffff, (e->qflags >> 31) ? " SPLIT" : "");
}

static long long
hanglog_wall_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_REALTIME, &ts);
   return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
hanglog_lookup_addr(uint64_t addr)
{
   /* newest first */
   int printed = 0;
   uint64_t best_dist = UINT64_MAX;
   const struct tu_hanglog_bo *best = NULL;
   int64_t now = os_time_get_nano();
   for (uint32_t i = 0; i < TU_HANGLOG_BO_RING && printed < 6; i++) {
      const struct tu_hanglog_bo *b = &tu_hanglog_bos[i];
      uint8_t op = __atomic_load_n(&b->op, __ATOMIC_ACQUIRE);
      if (!op)
         continue;
      if (addr >= b->iova && addr < b->iova + b->size) {
         mesa_logw("TU_HANGLOG:       addr in BO %s iova 0x%" PRIx64 " size 0x%" PRIx64 " (%s, %.3f s ago) offset 0x%" PRIx64,
                   op == 1 ? "ALLOC" : op == 2 ? "FREE" : "IMPORT",
                   b->iova, b->size, b->name, (now - b->t_ns) / 1e9, addr - b->iova);
         printed++;
      } else if (b->iova > addr && b->iova - addr < best_dist) {
         best_dist = b->iova - addr;
         best = b;
      } else if (b->iova + b->size <= addr && addr - (b->iova + b->size) < best_dist) {
         best_dist = addr - (b->iova + b->size);
         best = b;
      }
   }
   if (!printed) {
      mesa_logw("TU_HANGLOG:       addr not inside any recorded BO");
      if (best)
         mesa_logw("TU_HANGLOG:       nearest BO iova 0x%" PRIx64 " size 0x%" PRIx64 " (%s op %u) distance 0x%" PRIx64,
                   best->iova, best->size, best->name, best->op, best_dist);
   }
}

static const struct tu_hanglog_submit *
hanglog_find_submit(uint32_t ctx, uint32_t ts)
{
   const struct tu_hanglog_submit *hs =
      &tu_hanglog_submits[ts & (TU_HANGLOG_SUBMIT_RING - 1)];
   if (hs->ts != ts || hs->ctx != ctx)
      return NULL;
   return hs;
}

/* Print what went into kgsl timestamp ts. detail: also list pass-level
 * events of each command buffer, and the tail of draws.
 */
static void
hanglog_print_submit(const char *tag, uint32_t ctx, uint32_t ts,
                     uint32_t marker, bool detail)
{
   const struct tu_hanglog_submit *hs = hanglog_find_submit(ctx, ts);
   if (!hs) {
      mesa_logw("TU_HANGLOG:   [%s] submit ts %u: not recorded (aux/bind or overwritten)", tag, ts);
      return;
   }
   mesa_logw("TU_HANGLOG:   [%s] submit ts %u: %u cmdbuf(s)%s, %.3f s ago",
             tag, ts, hs->ncmds, hs->ncmds > hs->n ? " (truncated)" : "",
             (os_time_get_nano() - hs->t_ns) / 1e9);
   mesa_logw("TU_HANGLOG:   [%s] draws %u: FS writes depth %u, FS stencil export %u, FS discard %u",
             tag, hs->draws, hs->fsz, hs->fsst, hs->kill);
   for (uint32_t c = 0; c < hs->n; c++) {
      bool has_marker = marker && marker >= hs->first[c] && marker <= hs->last[c];
      mesa_logw("TU_HANGLOG:     cmdbuf#%u ids %u..%u%s", hs->seq[c],
                hs->first[c], hs->last[c], has_marker ? "  <-- CP marker inside" : "");
      if (!detail || !hs->first[c])
         continue;
      uint32_t first = hs->first[c], last = hs->last[c];
      if (last - first >= TU_HANGLOG_RING)
         first = last - TU_HANGLOG_RING + 1;
      unsigned draws = 0, disp = 0, shown = 0;
      for (uint32_t id = first; id && id <= last; id++) {
         const struct tu_hanglog_entry *e = &tu_hanglog_ring[id & (TU_HANGLOG_RING - 1)];
         if (e->id != id || e->cmd_seq != hs->seq[c])
            continue;
         if (e->kind <= 2)
            draws++;
         else if (e->kind <= 4)
            disp++;
         else if (shown < 60) {
            hanglog_print_entry("      ev", id);
            shown++;
         }
      }
      mesa_logw("TU_HANGLOG:       cmdbuf#%u: %u draws, %u dispatches", hs->seq[c], draws, disp);
      /* last few draws/dispatches of this command buffer */
      unsigned tail = 0;
      for (uint32_t id = last; id >= first && id && tail < 8; id--) {
         const struct tu_hanglog_entry *e = &tu_hanglog_ring[id & (TU_HANGLOG_RING - 1)];
         if (e->id != id || e->cmd_seq != hs->seq[c])
            continue;
         hanglog_print_entry("      tail", id);
         tail++;
      }
   }
}

static void
hanglog_check_query_copy(struct tu_device *dev, uint32_t marker)
{
   const struct tu_hanglog_entry *e =
      &tu_hanglog_ring[marker & (TU_HANGLOG_RING - 1)];
   if (e->id == marker && e->kind == 31)
      tu_hanglog_dump_query_copy(dev, e, hanglog_print_entry);
}


/* Compact map of every marked command in a submit with the IB chunk it was
 * emitted into, so a kernel fault report (IB1/IB2 base + remaining size) can
 * be matched to a draw.
 */
static void
hanglog_dump_ib_map(uint32_t ctx, uint32_t ts)
{
   const struct tu_hanglog_submit *hs = hanglog_find_submit(ctx, ts);
   if (!hs)
      return;
   unsigned lines = 0;
   for (uint32_t c = 0; c < hs->n && lines < 8000; c++) {
      uint32_t first = hs->first[c], last = hs->last[c];
      if (!first)
         continue;
      if (last - first >= TU_HANGLOG_RING)
         first = last - TU_HANGLOG_RING + 1;
      for (uint32_t id = first; id && id <= last && lines < 8000; id++) {
         const struct tu_hanglog_entry *e = &tu_hanglog_ring[id & (TU_HANGLOG_RING - 1)];
         if (e->id != id || e->cmd_seq != hs->seq[c])
            continue;
         if (e->kind <= 2)
            mesa_logw("TU_HANGLOG: IBMAP ts %u id %u k%u #%u rp%u ib 0x%" PRIx64 "+0x%x vs=%s fs=%s n=%u inst=%u uav=0x%x %s first=%u max=%u voff=%d isz=%u topo=%u restart=%u",
                      ts, id, e->kind, e->cmd_seq, e->rp, e->ib_start, e->ib_off * 4,
                      e->vs, e->fs, e->count, e->instances, e->uav_mask,
                      (e->qflags & 1) ? "IDX" : "seq", e->qfirst, e->qcount,
                      (int32_t) e->qtype, (e->qflags >> 1) & 7, (e->qflags >> 4) & 0xff,
                      (e->qflags >> 12) & 1);
         else
            mesa_logw("TU_HANGLOG: IBMAP ts %u id %u k%u #%u rp%u ib 0x%" PRIx64 "+0x%x vs=%s fs=%s cs=%s n=%u uav=0x%x",
                      ts, id, e->kind, e->cmd_seq, e->rp, e->ib_start, e->ib_off * 4,
                      e->kind <= 4 ? e->vs : "", e->kind <= 4 ? e->fs : "",
                      e->kind <= 4 ? e->cs : "", e->kind <= 4 ? e->count : 0,
                      e->kind <= 4 ? e->uav_mask : 0);
         lines++;
      }
   }
}

static void
hanglog_query_faults(struct tu_device *dev, uint32_t ctx)
{
   struct kgsl_context_property_fault f = {};
   struct kgsl_context_property cp = {
      .data = (uint64_t)(uintptr_t)&f,
      .size = sizeof(f),
      .type = KGSL_CONTEXT_PROP_FAULTS,
      .contextid = ctx,
   };
   if (get_kgsl_prop(dev->fd, KGSL_PROP_CONTEXT_PROPERTY, &cp, sizeof(cp)) == VK_SUCCESS)
      mesa_logw("TU_HANGLOG:   ctx %u kernel fault count %d (last fault ts %u)", ctx, f.faults, f.timestamp);
   else
      mesa_logw("TU_HANGLOG:   ctx %u fault count query failed (%s)", ctx, strerror(errno));

   struct kgsl_fault_report rep = { .context_id = ctx };
   if (safe_ioctl(dev->fd, IOCTL_KGSL_GET_FAULT_REPORT, &rep)) {
      mesa_logw("TU_HANGLOG:   fault report unsupported (%s)", strerror(errno));
      return;
   }
   if (!rep.faultnents) {
      mesa_logw("TU_HANGLOG:   fault report: none");
      return;
   }
   uint32_t n = MIN2(rep.faultnents, 4u);
   struct kgsl_fault faults[4] = {};
   rep.faultlist = (uint64_t)(uintptr_t)faults;
   rep.faultnents = n;
   rep.faultsize = sizeof(struct kgsl_fault);
   if (safe_ioctl(dev->fd, IOCTL_KGSL_GET_FAULT_REPORT, &rep)) {
      mesa_logw("TU_HANGLOG:   fault report read failed (%s)", strerror(errno));
      return;
   }
   for (uint32_t i = 0; i < n; i++) {
      mesa_logw("TU_HANGLOG:   fault type %u count %u", faults[i].type, faults[i].count);
      if (faults[i].type == KGSL_FAULT_TYPE_PAGEFAULT && faults[i].count) {
         uint32_t c = MIN2(faults[i].count, 4u);
         struct kgsl_pagefault_report pf[4] = {};
         faults[i].fault = (uint64_t)(uintptr_t)pf;
         faults[i].count = c;
         faults[i].size = sizeof(pf[0]);
         struct kgsl_fault one = faults[i];
         struct kgsl_fault_report r2 = {
            .faultlist = (uint64_t)(uintptr_t)&one, .faultnents = 1,
            .faultsize = sizeof(one), .context_id = ctx,
         };
         if (!safe_ioctl(dev->fd, IOCTL_KGSL_GET_FAULT_REPORT, &r2)) {
            for (uint32_t j = 0; j < c; j++)
            {
               mesa_logw("TU_HANGLOG:     pagefault addr 0x%" PRIx64 " type 0x%x%s%s%s%s",
                         (uint64_t) pf[j].fault_addr, pf[j].fault_type,
                         (pf[j].fault_type & KGSL_PAGEFAULT_TYPE_READ) ? " READ" : "",
                         (pf[j].fault_type & KGSL_PAGEFAULT_TYPE_WRITE) ? " WRITE" : "",
                         (pf[j].fault_type & KGSL_PAGEFAULT_TYPE_TRANSLATION) ? " TRANSLATION" : "",
                         (pf[j].fault_type & KGSL_PAGEFAULT_TYPE_PERMISSION) ? " PERMISSION" : "");
               hanglog_lookup_addr(pf[j].fault_addr);
            }
         } else {
            mesa_logw("TU_HANGLOG:     pagefault details read failed (%s)", strerror(errno));
         }
      }
   }
}

struct hanglog_qstate {
   int32_t last_faults;
   uint32_t last_fault_ts;
   uint32_t last_retired;
   int64_t since_ns;
   bool reported;
   int64_t reported_at;
};

static void *
hanglog_thread(void *data)
{
   struct tu_device *dev = (struct tu_device *) data;
   struct hanglog_qstate st[TU_MAX_QUEUE_FAMILIES][8] = {};
   const int64_t threshold = 2000000000ll;
   mesa_logw("TU_HANGLOG: watchdog running (pid %d)", (int) getpid());
   {
      /* Only settings-like variables; nothing that could carry account data. */
      static const char *prefixes[] = {
         "TU_", "IR3_", "FD_", "MESA_", "FEX_", "DXVK_", "VKD3D_", "PROTON_",
         "WINEESYNC", "WINEFSYNC", "WINE_DISABLE", "WINEDLLOVERRIDES",
         "heap_memory_percent", "tu_", "vk_", "radv_", "ntsync", "DROIDDECK_",
      };
      extern char **environ;
      for (char **e = environ; e && *e; e++) {
         for (unsigned i = 0; i < ARRAY_SIZE(prefixes); i++) {
            if (!strncmp(*e, prefixes[i], strlen(prefixes[i]))) {
               mesa_logw("TU_HANGLOG: env %s", *e);
               break;
            }
         }
      }
   }

   bool lost_reported = false;
   int64_t last_beat = 0;
   while (!dev->hanglog_stop.load(std::memory_order_acquire)) {
      usleep(250 * 1000);
      int64_t now = os_time_get_nano();
      static int64_t last_stats = 0;
      if (now - last_stats > 60ll * 1000000000ll) {
         last_stats = now;
         const long long stat_t = hanglog_wall_ms();
         static const char *fn[] = { "vsuav", "fsuav", "marker", "indirect", "query",
            "timestamp", "barrier", "depthwrite", "blend", "many", "msaa" };
         for (int split = 1; split >= 0; split--) {
            for (int sm = 1; sm >= 0; sm--) {
               for (uint32_t m = 0; m < TU_FEAT_STATS; m++) {
                  uint32_t c = tu_feat_stats[split][sm][m].load(std::memory_order_relaxed);
                  if (!c)
                     continue;
                  char names[160] = "";
                  for (unsigned b = 0; b < ARRAY_SIZE(fn); b++)
                     if (m & (1u << b)) {
                        strncat(names, fn[b], sizeof(names) - strlen(names) - 2);
                        strncat(names, ",", sizeof(names) - strlen(names) - 1);
                     }
                  mesa_logw("TU_HANGLOG: t=%lld passstat %s %s feat 0x%03x {%s} count %u",
                            stat_t, split ? "SPLIT" : "plain",
                            sm ? "SYSMEM" : "GMEM", m, names, c);
               }
            }
         }
      }
      if (now - last_beat > 10ll * 1000000000ll) {
         last_beat = now;
         for (unsigned i = 0; i < TU_MAX_QUEUE_FAMILIES; i++)
            for (int q = 0; q < MIN2(dev->queue_count[i], 8); q++) {
               struct tu_queue *queue = &dev->queues[i][q];
               if (vk_queue_is_emulated(&queue->vk) ||
                   !queue->hanglog_has_submit.load(std::memory_order_acquire))
                  continue;
               struct kgsl_cmdstream_readtimestamp_ctxtid r = {
                  .context_id = queue->msm_queue_id,
                  .type = KGSL_TIMESTAMP_RETIRED,
               };
               safe_ioctl(dev->fd, IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID, &r);
               mesa_logw("TU_HANGLOG: t=%lld beat ctx %u retired %u submitted %u marker %u",
                         hanglog_wall_ms(), queue->msm_queue_id, r.timestamp,
                         queue->hanglog_last_ts.load(std::memory_order_relaxed),
                         dev->global_bo_map->hanglog_marker);
            }
      }
      if (!lost_reported && vk_device_is_lost_no_report(&dev->vk)) {
         lost_reported = true;
         uint32_t marker = dev->global_bo_map->hanglog_marker;
         mesa_logw("TU_HANGLOG: t=%lld DEVICE LOST reported to the app; CP marker %u", hanglog_wall_ms(), marker);
         for (int k = 12; k >= 1; k--)
            if (marker > (uint32_t) k)
               hanglog_print_entry("before", marker - k);
         hanglog_print_entry("AT", marker);
         for (unsigned i = 0; i < TU_MAX_QUEUE_FAMILIES; i++)
            for (int q = 0; q < MIN2(dev->queue_count[i], 8); q++)
               if (!vk_queue_is_emulated(&dev->queues[i][q].vk))
                  hanglog_query_faults(dev, dev->queues[i][q].msm_queue_id);
      }
      for (unsigned i = 0; i < TU_MAX_QUEUE_FAMILIES; i++) {
         for (int q = 0; q < MIN2(dev->queue_count[i], 8); q++) {
            struct tu_queue *queue = &dev->queues[i][q];
            if (vk_queue_is_emulated(&queue->vk) ||
                !queue->hanglog_has_submit.load(std::memory_order_acquire))
               continue;
            struct hanglog_qstate *s = &st[i][q];
            if (!s->since_ns)
               s->since_ns = os_time_get_nano();
            uint32_t submitted = queue->hanglog_last_ts.load(std::memory_order_relaxed);
            {
               struct kgsl_context_property_fault f = {};
               struct kgsl_context_property cp = {
                  .data = (uint64_t)(uintptr_t)&f,
                  .size = sizeof(f),
                  .type = KGSL_CONTEXT_PROP_FAULTS,
                  .contextid = queue->msm_queue_id,
               };
               if (get_kgsl_prop(dev->fd, KGSL_PROP_CONTEXT_PROPERTY, &cp, sizeof(cp)) == VK_SUCCESS &&
                   (f.faults != s->last_faults || f.timestamp != s->last_fault_ts)) {
                  struct kgsl_cmdstream_readtimestamp_ctxtid r0 = {
                     .context_id = queue->msm_queue_id,
                     .type = KGSL_TIMESTAMP_RETIRED,
                  };
                  safe_ioctl(dev->fd, IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID, &r0);
                  uint32_t marker = dev->global_bo_map->hanglog_marker;
                  mesa_logw("TU_HANGLOG: t=%lld ctx %u NEW KERNEL FAULT: count %d -> %d, fault ts %u (retired %u, submitted %u, CP marker %u)",
                            hanglog_wall_ms(), queue->msm_queue_id, s->last_faults, f.faults,
                            f.timestamp, r0.timestamp, submitted, marker);
                  s->last_faults = f.faults;
                  s->last_fault_ts = f.timestamp;
                  hanglog_print_submit("fault-1", queue->msm_queue_id, f.timestamp - 1, 0, false);
                  hanglog_print_submit("FAULT", queue->msm_queue_id, f.timestamp, marker, true);
                  hanglog_dump_ib_map(queue->msm_queue_id, f.timestamp);
                  hanglog_print_submit("fault+1", queue->msm_queue_id, f.timestamp + 1, marker, false);
                  hanglog_print_entry("marker", marker);
                  hanglog_check_query_copy(dev, marker);
                  hanglog_query_faults(dev, queue->msm_queue_id);
               }
            }
            struct kgsl_cmdstream_readtimestamp_ctxtid rd = {
               .context_id = queue->msm_queue_id,
               .type = KGSL_TIMESTAMP_RETIRED,
            };
            if (safe_ioctl(dev->fd, IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID, &rd))
               continue;
            bool idle = timestamp_cmp(rd.timestamp, submitted);
            if (idle || rd.timestamp != s->last_retired) {
               if (s->reported) {
                  mesa_logw("TU_HANGLOG: t=%lld ctx %u RESUMED after %.1f s (retired %u, submitted %u, marker %u)",
                            hanglog_wall_ms(), queue->msm_queue_id, (now - s->since_ns) / 1e9,
                            rd.timestamp, submitted, dev->global_bo_map->hanglog_marker);
                  hanglog_query_faults(dev, queue->msm_queue_id);
               }
               s->last_retired = rd.timestamp;
               s->since_ns = now;
               s->reported = false;
               continue;
            }
            if (!s->reported && now - s->since_ns > threshold) {
               s->reported = true;
               uint32_t marker = dev->global_bo_map->hanglog_marker;
               mesa_logw("TU_HANGLOG: t=%lld ctx %u STALLED %.1f s: retired ts %u, submitted ts %u, CP marker %u (GMEM_FOR passes so far: %u)",
                         hanglog_wall_ms(), queue->msm_queue_id, (now - s->since_ns) / 1e9,
                         rd.timestamp, submitted, marker,
                         tu_gmem_for_hits.load(std::memory_order_relaxed));
               for (int k = 12; k >= 1; k--)
                  if (marker > (uint32_t) k)
                     hanglog_print_entry("before", marker - k);
               hanglog_print_entry("AT", marker);
               hanglog_check_query_copy(dev, marker);
               hanglog_query_faults(dev, queue->msm_queue_id);
               hanglog_print_submit("retired", queue->msm_queue_id, rd.timestamp, marker, false);
               hanglog_print_submit("STUCK", queue->msm_queue_id, rd.timestamp + 1, marker, true);
               hanglog_print_submit("next", queue->msm_queue_id, rd.timestamp + 2, marker, false);
            }
         }
      }
   }
   return NULL;
}

void
tu_kgsl_hanglog_start(struct tu_device *dev)
{
   if (!tu_hanglog_enabled)
      return;
   dev->hanglog_stop.store(false);
   if (pthread_create(&dev->hanglog_thread, NULL, hanglog_thread, dev) == 0)
      dev->hanglog_running = true;
}

void
tu_kgsl_hanglog_stop(struct tu_device *dev)
{
   if (!dev->hanglog_running)
      return;
   dev->hanglog_stop.store(true, std::memory_order_release);
   pthread_join(dev->hanglog_thread, NULL);
   dev->hanglog_running = false;
}

static VkResult
kgsl_device_check_status(struct tu_device *device)
{
   for (unsigned i = 0; i < TU_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++) {
         /* Emulated queues share the real queue's context and have no
          * kernel submitqueue of their own, so skip them.
          */
         if (vk_queue_is_emulated(&device->queues[i][q].vk))
            continue;

         /* KGSL's KGSL_PROP_GPU_RESET_STAT takes the u32 msm_queue_id and returns a
         * KGSL_CTX_STAT_* for the worst reset that happened since the last time it
         * was queried on that queue.
         */
         uint32_t value = device->queues[i][q].msm_queue_id;
         VkResult status = get_kgsl_prop(device->fd, KGSL_PROP_GPU_RESET_STAT,
                                       &value, sizeof(value));
         if (status != VK_SUCCESS)
            return vk_device_set_lost(&device->vk, "Failed to get GPU reset status");

         if (value != KGSL_CTX_STAT_NO_ERROR &&
            value != KGSL_CTX_STAT_INNOCENT_CONTEXT_RESET_EXT) {
            if (tu_hanglog_enabled || kgsl_ignore_faults())
               mesa_logw("TU_HANGLOG: reset status for ctx %u = 0x%x, marker %u%s",
                         device->queues[i][q].msm_queue_id, value,
                         device->global_bo_map->hanglog_marker,
                         kgsl_ignore_faults() ? " -> ignored" : "");
            if (kgsl_ignore_faults())
               continue;
            return vk_device_set_lost(&device->vk, "GPU faulted or hung");
         }
      }
   }

   return VK_SUCCESS;
}

static const struct tu_knl kgsl_knl_funcs = {
      .name = "kgsl",

      .device_init = kgsl_device_init,
      .device_finish = kgsl_device_finish,
      .device_get_gpu_timestamp = kgsl_device_get_gpu_timestamp,
      .device_get_suspend_count = kgsl_device_get_suspend_count,
      .device_check_status = kgsl_device_check_status,
      .submitqueue_new = kgsl_submitqueue_new,
      .submitqueue_close = kgsl_submitqueue_close,
      .bo_init = kgsl_bo_init,
      .bo_init_dmabuf = kgsl_bo_init_dmabuf,
      .bo_export_dmabuf = kgsl_bo_export_dmabuf,
      .bo_map = kgsl_bo_map,
      .bo_allow_dump = kgsl_bo_allow_dump,
      .bo_finish = kgsl_bo_finish,
      .submit_create = kgsl_submit_create,
      .submit_finish = kgsl_submit_finish,
      .submit_add_entries = kgsl_submit_add_entries,
      .submit_add_bind = kgsl_submit_add_bind,
      .queue_submit = kgsl_queue_submit,
      .queue_wait_fence = kgsl_queue_wait_fence,
      .sparse_vma_init = kgsl_sparse_vma_init,
      .sparse_vma_finish = kgsl_sparse_vma_finish,
};

static bool
tu_kgsl_get_raytracing(int fd)
{
   uint32_t value;
   int ret = get_kgsl_prop(fd, KGSL_PROP_IS_RAYTRACING_ENABLED, &value, sizeof(value));
   if (ret)
      return false;

   return value;
}

VkResult
tu_knl_kgsl_load(struct tu_instance *instance, int fd)
{
   if (instance->vk.enabled_extensions.KHR_display) {
      return vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                       "I can't KHR_display");
   }

   struct tu_physical_device *device = static_cast<struct tu_physical_device *>(
      vk_zalloc(&instance->vk.alloc, sizeof(*device), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE));
   if (!device)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   static const char dma_heap_path[] = "/dev/dma_heap/system";
   static const char ion_path[] = "/dev/ion";
   int dma_fd;

   dma_fd = open(dma_heap_path, O_RDONLY);
   if (dma_fd >= 0) {
      device->kgsl_dma_type = TU_KGSL_DMA_TYPE_DMAHEAP;
   } else {
      dma_fd = open(ion_path, O_RDONLY);
      if (dma_fd >= 0) {
         /* ION_IOC_FREE available only for legacy ION */
         struct ion_handle_data free = { .handle = 0 };
         if (safe_ioctl(dma_fd, ION_IOC_FREE, &free) >= 0 || errno != ENOTTY)
            device->kgsl_dma_type = TU_KGSL_DMA_TYPE_ION_LEGACY;
         else
            device->kgsl_dma_type = TU_KGSL_DMA_TYPE_ION;
      } else {
         mesa_logw(
            "Unable to open neither %s nor %s, VK_KHR_external_memory_fd would be "
            "unavailable: %s",
            dma_heap_path, ion_path, strerror(errno));
      }
   }

   VkResult result = VK_ERROR_INITIALIZATION_FAILED;

   struct kgsl_devinfo info;
   if (get_kgsl_prop(fd, KGSL_PROP_DEVICE_INFO, &info, sizeof(info)))
      goto fail;

   uint64_t gmem_iova;
   if (get_kgsl_prop(fd, KGSL_PROP_UCHE_GMEM_VADDR, &gmem_iova, sizeof(gmem_iova)))
      goto fail;

   uint32_t highest_bank_bit;
   if (get_kgsl_prop(fd, KGSL_PROP_HIGHEST_BANK_BIT, &highest_bank_bit,
                     sizeof(highest_bank_bit)))
      goto fail;

   uint32_t ubwc_version;
   if (get_kgsl_prop(fd, KGSL_PROP_UBWC_MODE, &ubwc_version,
                     sizeof(ubwc_version)))
      goto fail;

   if (get_kgsl_prop(fd, KGSL_PROP_UCHE_TRAP_BASE, &device->uche_trap_base,
                     sizeof(device->uche_trap_base))) {
      /* It is known to be hardcoded to */
      device->uche_trap_base = 0x1fffffffff000ull;
   }

   /* kgsl version check? */

   device->instance = instance;
   device->master_fd = -1;
   device->local_fd = fd;
   device->kgsl_dma_fd = dma_fd;

   device->dev_id.gpu_id =
      ((info.chip_id >> 24) & 0xff) * 100 +
      ((info.chip_id >> 16) & 0xff) * 10 +
      ((info.chip_id >>  8) & 0xff);
   device->dev_id.chip_id = info.chip_id;
   device->gmem_size = debug_get_num_option("TU_GMEM", info.gmem_sizebytes);
   device->gmem_base = gmem_iova;

   device->has_raytracing = tu_kgsl_get_raytracing(fd);

   device->submitqueue_priority_count = 1;

   /* Compat layers (e.g. proot/gamescope setups on Android) provide a DRM
    * render node next to kgsl and expect VK_EXT_physical_device_drm so that
    * wlroots/gamescope can find it. Report the first render/primary node we
    * can see, or the ones given by TU_KGSL_DRM_RENDER_NODE /
    * TU_KGSL_DRM_PRIMARY_NODE.
    */
   {
      struct stat st;
      const char *rn = getenv("TU_KGSL_DRM_RENDER_NODE");
      const char *pn = getenv("TU_KGSL_DRM_PRIMARY_NODE");
      char path[300];
      if (rn && stat(rn, &st) == 0 && S_ISCHR(st.st_mode)) {
         device->has_local = true;
         device->local_major = major(st.st_rdev);
         device->local_minor = minor(st.st_rdev);
      }
      if (pn && stat(pn, &st) == 0 && S_ISCHR(st.st_mode)) {
         device->has_master = true;
         device->master_major = major(st.st_rdev);
         device->master_minor = minor(st.st_rdev);
      }
      DIR *dir = opendir("/dev/dri");
      if (dir) {
         struct dirent *e;
         while ((e = readdir(dir))) {
            bool is_render = strncmp(e->d_name, "renderD", 7) == 0;
            bool is_card = strncmp(e->d_name, "card", 4) == 0;
            if (!is_render && !is_card)
               continue;
            snprintf(path, sizeof(path), "/dev/dri/%s", e->d_name);
            if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode))
               continue;
            if (is_render && !device->has_local) {
               device->has_local = true;
               device->local_major = major(st.st_rdev);
               device->local_minor = minor(st.st_rdev);
            } else if (is_card && !device->has_master) {
               device->has_master = true;
               device->master_major = major(st.st_rdev);
               device->master_minor = minor(st.st_rdev);
            }
         }
         closedir(dir);
      }
      /* gamescope insists on a primary node; compat layers usually only
       * provide a render node, so report it for both. */
      if (device->has_local && !device->has_master) {
         device->has_master = true;
         device->master_major = device->local_major;
         device->master_minor = device->local_minor;
      }
      if (device->has_local)
         mesa_logi("kgsl: reporting DRM render node %u:%u",
                   (unsigned) device->local_major, (unsigned) device->local_minor);
   }

   device->timeline_type = vk_sync_timeline_get_type(&vk_kgsl_sync_type);

   device->sync_types[0] = &vk_kgsl_sync_type;
   device->sync_types[1] = &device->timeline_type.sync;
   device->sync_types[2] = NULL;

   device->heap.size = tu_get_system_heap_size(device);
   device->heap.used = 0u;
   device->heap.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

   device->has_set_iova = kgsl_is_memory_type_supported(
      fd, KGSL_MEMFLAGS_USE_CPU_MAP);

   device->has_iova_align = true;

   /* Even if kernel is new enough, the GPU itself may not support it. */
   device->has_cached_coherent_memory = kgsl_is_memory_type_supported(
      fd, KGSL_MEMFLAGS_IOCOHERENT |
             (KGSL_CACHEMODE_WRITEBACK << KGSL_CACHEMODE_SHIFT));

   device->has_sparse = kgsl_is_virtual_bo_supported(fd);
   device->has_sparse_prr = device->has_sparse;
   device->has_lazy_bos = device->has_sparse;
   get_kgsl_prop(fd, KGSL_PROP_GPU_VA64_SIZE, &device->va_size,
                 sizeof(device->va_size));
   /* We don't actually use the VMA, but set a fake offset so that it doesn't
    * think we're trying to allocate 0 and assert.
    */
   device->va_start = 0x100000000;


   /* preemption is always supported on kgsl */
   device->has_preemption = true;

   /* KGSL doesn't allow writing the perf counter selector as the expectation is to use the uAPI provided for this. */
   device->is_perf_cntr_selectable = false;

   device->ubwc_config.highest_bank_bit = highest_bank_bit;

   /* The other config values can be partially inferred from the UBWC version,
    * but kgsl also hardcodes overrides for specific a6xx versions that we
    * have to follow here. Yuck.
    */
   switch (ubwc_version) {
   case KGSL_UBWC_1_0:
      device->ubwc_config.bank_swizzle_levels = 0x7;
      device->ubwc_config.macrotile_mode = FDL_MACROTILE_4_CHANNEL;
      break;
   case KGSL_UBWC_2_0:
      device->ubwc_config.bank_swizzle_levels = 0x6;
      device->ubwc_config.macrotile_mode = FDL_MACROTILE_4_CHANNEL;
      break;
   case KGSL_UBWC_3_0:
      device->ubwc_config.bank_swizzle_levels = 0x6;
      device->ubwc_config.macrotile_mode = FDL_MACROTILE_4_CHANNEL;
      break;
   case KGSL_UBWC_4_0:
   case KGSL_UBWC_5_0:
   case KGSL_UBWC_6_0:
      device->ubwc_config.bank_swizzle_levels = 0x6;
      device->ubwc_config.macrotile_mode = FDL_MACROTILE_8_CHANNEL;
      break;
   default:
      result = vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED, "unknown UBWC version 0x%x", ubwc_version);
      goto fail;
   }

   /* kgsl unfortunately hardcodes some settings for certain GPUs and doesn't
    * expose them in the uAPI so hardcode them here to match.
    */
   if (device->dev_id.gpu_id == 663 || device->dev_id.gpu_id == 680) {
      device->ubwc_config.macrotile_mode = FDL_MACROTILE_8_CHANNEL;
   }
   if (device->dev_id.gpu_id == 663) {
      /* level2_swizzling_dis = 1 */
      device->ubwc_config.bank_swizzle_levels = 0x4;
   }

   instance->knl = &kgsl_knl_funcs;

   result = tu_physical_device_init(device, instance);
   if (result != VK_SUCCESS)
      goto fail;

   list_addtail(&device->vk.link, &instance->vk.physical_devices.list);

   return VK_SUCCESS;

fail:
   vk_free(&instance->vk.alloc, device);
   if (dma_fd >= 0)
      close(dma_fd);
   return result;
}
