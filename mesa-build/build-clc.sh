#!/bin/bash
# SPIRV-LLVM-Translator（llvm-spirv + libLLVMSPIRVLib）和 libclc（spirv-mesa3d- / spirv64-mesa3d-）→ 私有前缀。
# 要在 build-llvm.sh 之后跑。
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/env.sh"
: "${MESA_BUILD:?set MESA_BUILD in .env}"
set -ex
cd $MESA_BUILD
PREFIX=$MESA_BUILD/prefix
J=${J:-16}
export PATH=$PREFIX/bin:$MESA_BUILD/venv/bin:$PATH
export LD_LIBRARY_PATH=$PREFIX/lib:$PREFIX/lib64

cmake -G Ninja -S src/SPIRV-LLVM-Translator -B build/llvm-spirv -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_INSTALL_PREFIX=$PREFIX -DLLVM_DIR=$PREFIX/lib/cmake/llvm \
  -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR=$MESA_BUILD/src/SPIRV-Headers \
  -DLLVM_SPIRV_INCLUDE_TESTS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF
ninja -C build/llvm-spirv -j$J install

cmake -G Ninja -S src/llvm-full/libclc -B build/libclc -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=$PREFIX -DLLVM_DIR=$PREFIX/lib/cmake/llvm \
  -DCMAKE_C_COMPILER=$PREFIX/bin/clang -DCMAKE_CXX_COMPILER=$PREFIX/bin/clang++ \
  -DLIBCLC_TARGETS_TO_BUILD="spirv-mesa3d-;spirv64-mesa3d-"
ninja -C build/libclc -j$J install
echo CLC-DONE
