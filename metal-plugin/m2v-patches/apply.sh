#!/bin/bash
# 在构建机上跑。准备桥库用的 metal2vulkan 源码树：钉住的 720ef1ce + 上游 AdrenoMetal/bridge/patches 的全部补丁（0001-0037，与上游
# scripts/build-adreno-metal-bridge.py 的顺序一致）+ 本板子的一处改动（池块上限 8 MiB，见下）。
#   用法：./apply.sh [目录]   默认 $M2V（先 git clone --shared 原树再 checkout 720ef1ce）
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../../env.sh"
DIR="${1:-${M2V:?set M2V in .env or pass the directory}}"
UP="${UP:-${UNIXNU:?set UNIXNU in .env}/drivers/AdrenoMetal}"
REV="${REV:-d6ca655}"           # AdrenoMetal 的提交（上游分支 feat/ipados27-gpu-parity，2026-10-29）
cd "$DIR"
[ "$(git rev-parse --short=8 HEAD)" = 720ef1ce ] || { echo "error: $DIR 不在 720ef1ce" >&2; exit 1; }
git checkout -q . && git clean -fdq
for f in $(git -C "$UP" ls-tree --name-only "$REV" bridge/patches/ | sort); do
	git -C "$UP" show "$REV:$f" | git apply - || { echo "error: $f 打不上" >&2; exit 1; }
done
/usr/bin/python3 - <<'PYEOF'
p = 'crates/metal2vulkan-runtime/src/memory.rs'
s = open(p).read()
old = "/// The size of a pooled block, at most the device's `maxMemoryAllocationSize`.\npub(super) const BLOCK_BYTES: u64 = 64 << 20;"
new = """/// The size of a pooled block, at most the device's `maxMemoryAllocationSize`.
///
/// EVB10 (Mali-G610 under UniXNU): 8 MiB, not 64. The kernel driver makes every allocation of a client of
/// physically contiguous, zeroed memory and maps it into the process, and a process lives under a memory limit
/// (200 MB for a lock screen poster): a poster extension's few textures took 331 to 550 MB of 64 MiB blocks
/// and was killed for it (jetsam), and each block cost 70 ms to make. Pools still start at FIRST_BLOCK_BYTES."""
new += "\npub(super) const BLOCK_BYTES: u64 = 8 << 20;"
assert old in s
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
# 本板子的补丁（本目录下 0100- 起的文件，按文件名顺序）：
#   0100  YCbCr 双平面纹理：平面字节没变就不再转换（ycbcr_memo.rs；M2V_YCBCR_MEMO=N 才生效，N=每次采样的 64 字节块数）
#   0101  YCbCr 双平面纹理的线性采样：M2V_YCBCR_FILTER=device 时用设备自己的滤波器采转换后的影子图，不再在片元着色器里逐位仿 Metal 的滤波
#         （bind/plan/slots.rs；一次采样约 570 条指令，板上 750 MHz 时整屏壁纸一遍从 22.5 ms 降到 7.9 ms；默认不变）
for f in "$HERE"/0*.patch; do
	[ -f "$f" ] || continue
	git apply "$f" || { echo "error: $f 打不上" >&2; exit 1; }
done
echo "== $(git diff --stat | tail -1)"
