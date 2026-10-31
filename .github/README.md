# turnip-a8xx

Mesa Turnip (Vulkan) for **Adreno 8xx on KGSL** with mitigations for the A8xx
**"RBBM: GPC error"** GPU faults, built for glibc Linux-on-Android setups such as
[DroidDeck](https://github.com/Droid-Deck/DroidDeck).

> **Experimental.** Tested only on a Lenovo Legion Y700 Gen4 (Snapdragon 8 Elite,
> Adreno 830) with Cyberpunk 2077 via DroidDeck → Proton → vkd3d-proton → FEX.
> Not affiliated with the Mesa project. Most of the code was written with an LLM
> (commits carry `Generated-by: LLM`). No warranty.

[한국어 설명은 아래에 있습니다.](#한국어)

## Quick start

1. Download `Turnip-…-a8xx-r<N>.zip` from [Releases](../../releases)
   (newest release = recommended). Not "Source code (zip)": that is the
   whole Mesa source, added by GitHub to every release.
2. Import it as the Linux (glibc) Vulkan driver in DroidDeck.
3. Set the environment variable:

   ```
   TU_SAFEMODE=1
   ```

   and keep DroidDeck's **"Turnip: sysmem"** toggle on (`TU_DEBUG=sysmem`).

`TU_SAFEMODE=1` turns on the whole mitigation set (see below). The resolved set
is logged once as `TU_SAFEMODE: window 3000 ms | TU_CTX_RECOVER=1 …`.

## What the mitigations do

The A8xx GPU raises "RBBM: GPC error" faults under load (root cause unknown; see
"What was ruled out"). KGSL invalidates a context after 3 faults in 2 s, which
normally ends in `VK_ERROR_DEVICE_LOST`.

| Option | Effect |
|---|---|
| `TU_SAFEMODE=<ms>` | After a kernel GPU fault, for `<ms>` (1 = 3000): one submission in flight, GMEM rendering, `CP_WAIT_FOR_ME` per draw. Repeated faults double the window (max 60 s); at 4× the base, also WFI per draw (level 2). |
| `TU_CTX_RECOVER=1` | When KGSL invalidates the context, create a new one and continue. Work in flight is dropped (brief glitch, possibly stale textures until reloaded). Dead contexts are destroyed. |
| `TU_GMEM_MERGED=1` | Render split (suspend/resume) dynamic-rendering passes in GMEM. |
| `TU_KGSL_PREEMPT=ring` | Ring-buffer preemption style for the KGSL context. |
| `TU_QUERY_NOWAIT=1` | Don't wait for query availability in `vkCmdCopyQueryPoolResults` (results written before a reset are lost; waiting would hang). |
| `TU_HANGLOG=lite` | Kernel fault polling + log: fault reports, 10 s heartbeat with GPU memory / RSS / swap / safe-mode share. |

`TU_SAFEMODE` accepts comma tokens to adjust the preset:
`ms=<n>`, `recover`/`norecover`, `merged[=<spec>]`/`nomerged`,
`preempt=ring|fine|none`, `hanglog[=lite|1]`/`nohanglog`, `nowait`/`nonowait`.
Example: `TU_SAFEMODE=1,norecover,preempt=fine`. A variable you set explicitly
(e.g. `TU_CTX_RECOVER=0`) always wins.

## Build

Ubuntu 24.04 x86_64, cross-compiling to aarch64:

```sh
a8xx/build.sh --deps r55      # installs deps (sudo), builds, writes out/Turnip-…-a8xx-r55.zip
a8xx/build.sh r55             # later builds
```

GitHub Actions (`.github/workflows/build.yml`) builds every push; pushing a tag
`r<N>` publishes a release with the zip.

## History and releases

- Base: Mesa `main` snapshot of 2026-10-24 (`27.0.0-devel`), single commit "base".
- Every later commit is one step of the investigation; the commit messages explain why.
- [`a8xx/tags.txt`](../a8xx/tags.txt) maps `r7`…`r54` to the exact commit each
  historical zip was built from (r2–r6 predate the history). The
  **publish-history** workflow (Actions tab → Run workflow) creates those tags
  and rebuilds the chosen ones as releases (`all` for every one). Creating
  those tags needs a personal access token in the repository secret
  `TAG_TOKEN` (fine-grained, this repository only, permissions
  *Contents: Read and write* and *Workflows: Read and write*), because the old
  commits still carry upstream Mesa's workflow files.
- Per-build notes: [`a8xx/BUILD-NOTES.txt`](../a8xx/BUILD-NOTES.txt).

## License

Mesa's licenses apply (mostly MIT, see `docs/license.rst`). This repository
contains no proprietary driver code.

---

## 한국어

Adreno 8xx(KGSL)용 Mesa Turnip 드라이버예요. A8xx에서 생기는 **"RBBM: GPC error"** GPU 폴트를 견디도록 수정했어요.

> **실험용이에요.**
> - Lenovo Legion Y700 4세대(Adreno 830)에서 DroidDeck으로 Cyberpunk 2077을 실행하는 환경에서만 검증했어요.
> - Mesa 공식 프로젝트와 관계없어요.
> - 코드 대부분은 LLM으로 작성했어요(커밋에 `Generated-by: LLM` 표시).

### 사용법

1. [Releases](../../releases)에서 최신 `Turnip-…-a8xx-rNN.zip`을 받아요. GitHub이 자동으로 붙이는 "Source code (zip)"은 Mesa 소스 전체라서 드라이버가 아니에요.
2. DroidDeck에서 Linux(glibc) Vulkan 드라이버로 가져와요.
3. 환경변수에 `TU_SAFEMODE=1` 하나만 넣어요. 안전 모드, 컨텍스트 복구, GMEM_MERGED, ring 선점, 쿼리 대기 방지, 로그가 한 번에 켜져요.
4. DroidDeck의 **"Turnip: sysmem"** 토글은 켠 상태를 추천해요.

### 원인 추적에서 배제된 것

다음 항목은 바꿔 봤지만 GPC 폴트가 없어지지 않았어요:
- 페이지 폴트
- 캐시 플러시
- CP 동기화(PFP/ME)
- LRZ, UBWC, 선점
- VPC 레이아웃
- PC_MODE_CNTL bit5, 텍스처 prefetch, 매직 레지스터(블롭과 동일)
- WinNative A8xx 수정
- DVFS 클럭 고정
- 모든 패스 GMEM 강제

근본 원인은 아직 몰라요. 하드웨어나 펌웨어 쪽일 가능성이 남아 있어요.

### 알려진 한계

- **메모리:** 12GB 기기에서는 Cyberpunk가 GPU 메모리만 약 4.5GB(1080p) 또는 약 3.8GB(720p)를 써요. 그래서 40~80분쯤 지나면 안드로이드가 메모리 부족으로 앱을 종료할 수 있어요.
- **화면 깨짐:** 컨텍스트를 복구하는 순간 처리 중이던 작업이 버려져서, 일시적으로 화면이 깨질 수 있어요. 세이브를 다시 불러오면 회복돼요.
- **성능:** 프레임은 CPU, 즉 FEX의 x86 변환 비용에 묶여 있어요. 해상도를 낮춰도 프레임은 거의 그대로예요.
