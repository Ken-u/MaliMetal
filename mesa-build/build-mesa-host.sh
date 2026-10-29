#!/bin/bash
# 在构建机上先编一份「host x86 的 PanVK」：
#   1. 证明依赖（LLVM/libclc/SPIRV-LLVM-Translator/SPIRV-Tools/glslang/libdrm）都凑齐了；
#   2. 顺带装出 mesa_clc / panfrost_compile 这些 host 工具，给后面 iOS 交叉编译用（-Dmesa-clc=system -Dprecomp-compiler=system）。
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/env.sh"
: "${MESA_BUILD:?set MESA_BUILD in .env}"
set -ex
cd $MESA_BUILD
PREFIX=$MESA_BUILD/prefix
J=${J:-24}
export PATH=$PREFIX/bin:$MESA_BUILD/venv/bin:$PATH
export PKG_CONFIG_PATH=$PREFIX/lib/x86_64-linux-gnu/pkgconfig:$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:$PREFIX/share/pkgconfig
export LD_LIBRARY_PATH=$PREFIX/lib:$PREFIX/lib64:$PREFIX/lib/x86_64-linux-gnu
export CC=/usr/bin/gcc CXX=/usr/bin/g++
# 环境里默认有个 LLVM_CONFIG 指向 UniXNU 内核工具链的替身，meson 会优先用它，必须盖掉
export LLVM_CONFIG=$PREFIX/bin/llvm-config
unset CLANG_BIN CLANGDWARF_BIN CLANGPDB_BIN

OPTS=(
  -Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms=
  -Dllvm=enabled -Dshared-llvm=enabled
  -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
  -Dtools= -Dvideo-codecs= -Dvulkan-layers= -Dxmlconfig=disabled -Dshader-cache=disabled
  -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dzstd=disabled -Dbuild-tests=false
  -Dbuildtype=release
)
if [ ! -f build/mesa-host/build.ninja ]; then
  meson setup build/mesa-host src/mesa --prefix=$PREFIX/mesa-host --wrap-mode=nodownload "${OPTS[@]}"
fi
ninja -C build/mesa-host -j$J install
echo MESA-HOST-DONE
