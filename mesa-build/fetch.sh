#!/bin/bash
# 拉 PanVK 构建需要的第三方源码（libclc / SPIRV-LLVM-Translator / glslang / SPIRV-Tools）。只下载，不改系统。
# 代理偶尔会掐 TLS，所以全部带重试；llvm 用 tarball + curl 断点续传。
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/env.sh"
: "${MESA_BUILD:?set MESA_BUILD in .env}"
cd $MESA_BUILD
mkdir -p src dl && cd src

retry() { local n=0; until "$@"; do n=$((n+1)); [ $n -ge 8 ] && return 1; echo "retry $n: $*"; sleep 5; done; }

if [ ! -d llvm-project/libclc ]; then
  retry curl -sSL -C - --retry 5 --retry-delay 3 -o ../dl/llvm-20.1.8.tar.gz \
    https://github.com/llvm/llvm-project/archive/refs/tags/llvmorg-20.1.8.tar.gz || exit 1
  mkdir -p llvm-project
  tar -xzf ../dl/llvm-20.1.8.tar.gz -C llvm-project --strip-components=1 \
    --wildcards 'llvm-project-llvmorg-20.1.8/libclc/*' 'llvm-project-llvmorg-20.1.8/cmake/*' 'llvm-project-llvmorg-20.1.8/llvm/cmake/*' || exit 1
fi

clone() { [ -d "$2/.git" ] || retry git clone --depth 1 "${@:3}" "$1" "$2"; }
clone https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git SPIRV-LLVM-Translator --branch llvm_release_200 || exit 1
clone https://github.com/KhronosGroup/SPIRV-Headers.git SPIRV-Headers || exit 1
clone https://github.com/KhronosGroup/SPIRV-Tools.git SPIRV-Tools || exit 1
clone https://github.com/KhronosGroup/glslang.git glslang --branch vulkan-sdk-1.4.357.0 || exit 1
echo FETCH-DONE
du -sh $MESA_BUILD/src/*
