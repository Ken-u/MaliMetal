#!/bin/bash
# 在构建机上从源码编一份 LLVM/Clang 20.1.8（host x86，只为 PanVK 的 mesa_clc / libclc / llvm-spirv 服务），装进私有前缀。
# AOSP 预编译 clang 没有 opt、而且 libLLVM 是 libc++ 静态打进去的，和 Mesa 的 C++ 混不到一起，所以自己编一份。
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/env.sh"
: "${MESA_BUILD:?set MESA_BUILD in .env}"
set -ex
cd $MESA_BUILD
PREFIX=$MESA_BUILD/prefix
J=${J:-24}
export PATH=$MESA_BUILD/venv/bin:$PATH

if [ ! -d src/llvm-full/llvm ]; then
  mkdir -p src/llvm-full
  tar -xzf dl/llvm-20.1.8.tar.gz -C src/llvm-full --strip-components=1 \
    --wildcards 'llvm-project-llvmorg-20.1.8/llvm/*' 'llvm-project-llvmorg-20.1.8/clang/*' \
                'llvm-project-llvmorg-20.1.8/cmake/*' 'llvm-project-llvmorg-20.1.8/third-party/*' \
                'llvm-project-llvmorg-20.1.8/libclc/*'
fi

cmake -G Ninja -S src/llvm-full/llvm -B build/llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/gcc -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_INSTALL_PREFIX=$PREFIX \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON -DCLANG_LINK_CLANG_DYLIB=ON \
  -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_RTTI=ON \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
  -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_ZLIB=ON \
  -DLLVM_INSTALL_UTILS=ON

ninja -C build/llvm -j$J install
echo LLVM-DONE
