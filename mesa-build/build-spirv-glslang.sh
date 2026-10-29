#!/bin/bash
# SPIRV-Headers + SPIRV-Tools + glslang → 私有前缀（host x86，gcc）。
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/env.sh"
: "${MESA_BUILD:?set MESA_BUILD in .env}"
set -ex
cd $MESA_BUILD
PREFIX=$MESA_BUILD/prefix
J=${J:-8}
export PATH=$MESA_BUILD/venv/bin:$PATH

cmake -G Ninja -S src/SPIRV-Headers -B build/spirv-headers -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX
ninja -C build/spirv-headers install

# SPIRV-Tools 要的 headers 路径
mkdir -p src/SPIRV-Tools/external
[ -e src/SPIRV-Tools/external/spirv-headers ] || ln -s $MESA_BUILD/src/SPIRV-Headers src/SPIRV-Tools/external/spirv-headers
cmake -G Ninja -S src/SPIRV-Tools -B build/spirv-tools -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX \
  -DSPIRV_SKIP_TESTS=ON -DSPIRV_WERROR=OFF -DSPIRV-Headers_SOURCE_DIR=$MESA_BUILD/src/SPIRV-Headers \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF
ninja -C build/spirv-tools -j$J install

# glslang 自己要 spirv-tools 在 External/spirv-tools
[ -e src/glslang/External/spirv-tools ] || ln -s $MESA_BUILD/src/SPIRV-Tools src/glslang/External/spirv-tools
cmake -G Ninja -S src/glslang -B build/glslang -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX \
  -DENABLE_OPT=ON -DALLOW_EXTERNAL_SPIRV_TOOLS=ON -DENABLE_GLSLANG_BINARIES=ON -DBUILD_TESTING=OFF \
  -DCMAKE_PREFIX_PATH=$PREFIX
ninja -C build/glslang -j$J install
echo SPIRV-GLSLANG-DONE
