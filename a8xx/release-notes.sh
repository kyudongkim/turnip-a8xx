#!/usr/bin/env bash
# Print the GitHub release notes for a tag r<N>: which zip is for which app,
# then that build's section of a8xx/BUILD-NOTES.txt.
set -euo pipefail
cd "$(dirname "$0")/.."
TAG="$1"
N="${TAG#r}"; [[ "$N" =~ ^[0-9]+$ ]] || N=0

if (( N >= 55 )); then
   LINUX_SET='Set `TU_SAFEMODE=1` and keep DroidDeck'"'"'s "Turnip: sysmem" toggle on.'
   ANDROID_SET='Mitigations are **on by default** (no environment variable needed; `TU_SAFEMODE=0` turns them off).'
   LINUX_KO='환경변수 `TU_SAFEMODE=1`을 넣고 DroidDeck의 "Turnip: sysmem" 토글을 켜 두세요.'
   ANDROID_KO='완화 기능이 **기본으로 켜져 있어요**. 환경변수가 필요 없고, 끄려면 `TU_SAFEMODE=0`을 넣어요.'
elif (( N >= 52 )); then
   LINUX_SET='Set `TU_SAFEMODE=1`.'
   ANDROID_SET='Mitigations are off by default in this build; set `TU_SAFEMODE=1` if your app allows environment variables.'
   LINUX_KO='환경변수 `TU_SAFEMODE=1`을 넣으세요.'
   ANDROID_KO='이 빌드는 완화 기능이 기본으로 꺼져 있어요. 앱에서 환경변수를 넣을 수 있으면 `TU_SAFEMODE=1`을 넣으세요.'
else
   LINUX_SET='Historical build: see the notes below for its options.'
   ANDROID_SET='Historical build: options are off by default; see the notes below.'
   LINUX_KO='과거 빌드예요. 옵션은 아래 노트를 참고하세요.'
   ANDROID_KO='과거 빌드예요. 옵션은 기본으로 꺼져 있고, 아래 노트를 참고하세요.'
fi

cat <<EOF
### Downloads

| File | Use with |
|---|---|
| \`Turnip-…-${TAG}-linux.zip\` | **DroidDeck** (glibc Linux on Android): import it as the Linux Vulkan driver. ${LINUX_SET} |
| \`Turnip-…-${TAG}-adrenotools.zip\` | **GameHub, GameNative** and other apps that load AdrenoTools driver packages. ${ANDROID_SET} |

"Source code (zip / tar.gz)" is the Mesa source added by GitHub, not a driver.
Verified only on Adreno 830 (Lenovo Legion Y700 Gen4) with DroidDeck; the AdrenoTools package is untested.

### 다운로드

| 파일 | 쓰는 곳 |
|---|---|
| \`…-linux.zip\` | **DroidDeck**: Linux Vulkan 드라이버로 가져오세요. ${LINUX_KO} |
| \`…-adrenotools.zip\` | **GameHub, GameNative**: AdrenoTools 드라이버로 가져오세요. ${ANDROID_KO} |

"Source code"는 Mesa 소스라서 드라이버가 아니에요. Adreno 830(Legion Y700 4세대)과 DroidDeck에서만 검증했고, AdrenoTools 패키지는 아직 검증 전이에요.

### Build notes

EOF
awk -v t="$TAG" '
  $0 ~ "^"t"([: ]|$)" {p=1; print; next}
  p && /^r[0-9]+([: ]|$)/ {exit}
  p {print}' a8xx/BUILD-NOTES.txt
