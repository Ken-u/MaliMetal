#!/bin/bash
# libdrm 2.4.123（host x86），只给 build-mesa-host.sh 里的 PanVK 编译用；iOS 目标用 ../darwin-drm 的垫片。
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/env.sh"
: "${MESA_BUILD:?set MESA_BUILD in .env}"
set -ex
cd $MESA_BUILD
PREFIX=$MESA_BUILD/prefix
export PATH=$MESA_BUILD/venv/bin:$PATH
mkdir -p dl src
[ -f dl/libdrm-2.4.123.tar.xz ] || curl -sSL -m 120 -o dl/libdrm-2.4.123.tar.xz https://dri.freedesktop.org/libdrm/libdrm-2.4.123.tar.xz
echo "a2b98567a149a74b0f50e91e825f9c0315d86e7be9b74394dae8b298caadb79e  dl/libdrm-2.4.123.tar.xz" | sha256sum -c -
[ -d src/libdrm-2.4.123 ] || tar -xJf dl/libdrm-2.4.123.tar.xz -C src
meson setup build/libdrm src/libdrm-2.4.123 --prefix=$PREFIX \
  -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled -Dvmwgfx=disabled -Domap=disabled \
  -Dexynos=disabled -Dfreedreno=disabled -Dtegra=disabled -Detnaviv=disabled -Dvc4=disabled \
  -Dcairo-tests=disabled -Dvalgrind=disabled -Dtests=false -Dman-pages=disabled --buildtype=release
ninja -C build/libdrm install
