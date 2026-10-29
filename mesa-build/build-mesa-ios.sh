#!/bin/bash
# 在构建机上把 PanVK 交叉编成 arm64-apple-ios17.0（目标是看 Darwin 移植要改多少，还没有 DRM 垫片，所以预期会在 libdrm / ioctl 上报错）。
# host 工具（mesa_clc / vtn_bindgen2 / panfrost_compile）用 build-mesa-host.sh 装好的那一份。
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/env.sh"
: "${MESA_BUILD:?set MESA_BUILD in .env}"
set -x
cd $MESA_BUILD
PREFIX=$MESA_BUILD/prefix
J=${J:-24}
SRC=${SRC:-src/mesa}
OUT=${OUT:-build/mesa-ios}
SDK=$IOS_SDK
A=$LLVM_BIN
export PATH=$PREFIX/mesa-host/bin:$PREFIX/bin:$MESA_BUILD/venv/bin:$PATH
# 注意：交叉编译不能带 host 的 PKG_CONFIG_PATH，否则会找到 host 的 x86 libdrm
unset PKG_CONFIG_PATH PKG_CONFIG_LIBDIR
export LD_LIBRARY_PATH=$PREFIX/lib:$PREFIX/lib64:$PREFIX/lib/x86_64-linux-gnu
# 环境里有 CCC_OVERRIDE_OPTIONS（UniXNU 内核构建塞的 +-Wno-unused-but-set-global），会让 meson 的所有编译探测全部失败
unset LLVM_CONFIG CLANG_BIN CLANGDWARF_BIN CLANGPDB_BIN CC CXX CCC_OVERRIDE_OPTIONS

SHIM=$MALIMETAL_ROOT/darwin-drm
SHIMOUT=$MESA_BUILD/build/darwin-drm
mkdir -p $SHIMOUT/pkgconfig
$A/clang-20 -target arm64-apple-ios17.0 -isysroot $SDK -O2 -Wall -Wextra -fPIC -I$SHIM/include -I$MESA_BUILD/$SRC/include \
  -I$MALI_DIR \
  -c $SHIM/drm_darwin.c -o $SHIMOUT/drm_darwin.o || exit 1
rm -f $SHIMOUT/libdrm_darwin.a && $A/llvm-ar rcs $SHIMOUT/libdrm_darwin.a $SHIMOUT/drm_darwin.o
sed -e "s|@PREFIX@|$SHIM|" -e "s|@LIBDIR@|$SHIMOUT|" $SHIM/pkgconfig/libdrm.pc.in > $SHIMOUT/pkgconfig/libdrm.pc

# clang 20 的 __has_builtin 对 GCC 专有的 __builtin_*_overflow_p 返回真，但真用就报 unknown builtin；强制走 Mesa 的纯 C 后备实现
FLAGS="'-target', 'arm64-apple-ios17.0', '-isysroot', '$SDK', '-I$SHIM/include', '-UHAVE___BUILTIN_ADD_OVERFLOW_P', '-UHAVE___BUILTIN_SUB_OVERFLOW_P'"
LFLAGS="$FLAGS, '-fuse-ld=lld', '-B$A', '-Wl,-adhoc_codesign', '-Wl,-S', '-Wl,-x', '-Wl,-map,$MESA_BUILD/panvk.map'"
cat > build/mesa-ios.cross <<EOF
[binaries]
c = ['$A/clang-20']
cpp = ['$A/clang-20', '--driver-mode=g++']
objc = ['$A/clang-20']
ar = '$A/llvm-ar'
ranlib = '$A/llvm-ranlib'
strip = '$A/llvm-strip'
pkg-config = '/usr/bin/pkg-config'
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
pkg_config_libdir = ['$SHIMOUT/pkgconfig']
[built-in options]
c_args = [$FLAGS]
cpp_args = [$FLAGS, '-stdlib=libc++']
objc_args = [$FLAGS]
c_link_args = [$LFLAGS]
cpp_link_args = [$LFLAGS, '-stdlib=libc++']
objc_link_args = [$LFLAGS]
EOF

OPTS=(
  -Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms=
  -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
  -Dshared-glapi=disabled -Dtools= -Dvideo-codecs= -Dvulkan-layers= -Dxmlconfig=disabled -Dshader-cache=disabled
  -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dzstd=disabled -Dexpat=disabled -Dbuild-tests=false
  -Dbuildtype=release -Db_ndebug=true
)
if [ ! -f $OUT/build.ninja ]; then
  meson setup $OUT $SRC --cross-file build/mesa-ios.cross --wrap-mode=nodownload "${OPTS[@]}" || exit 1
fi
# panvk_* 的弱入口点（Mach-O 上没有 weak alias）靠 dynamic_lookup 放过。不能放进 cross file / meson configure：
# 那样 meson 的 has_function 探测会全部「成功」（secure_getenv、sched_getaffinity、__builtin_*_overflow_p 都被误判存在）。
# 只在 ninja 阶段用 CCC_OVERRIDE_OPTIONS 追加给 clang 驱动。
export CCC_OVERRIDE_OPTIONS='# +-Wl,-undefined,dynamic_lookup'
ninja -C $OUT -j$J -k 0 src/panfrost/vulkan/libvulkan_panfrost.dylib
rc=$?
# 那 6645 个弱入口点在 dylib 里是 flat-namespace 弱导入：dyld 逐个遍历进程里每个镜像的导出 trie 去找，
# backboardd 里 0.8 s、poster 扩展（小核）里 1.4–2.6 s，主线程就耗在这里（gpu45）。改成「在本镜像里找」（找不到就是 NULL，
# 表项一样），原地改导入表并重做 ad hoc 签名（fix-weak-imports.py；绝对零符号的办法不行，lld 会给它做 rebase，gpu46）。
if [ $rc = 0 ]; then
  python3 $MALIMETAL_ROOT/mesa-build/fix-weak-imports.py $OUT/src/panfrost/vulkan/libvulkan_panfrost.dylib || rc=1
fi
echo MESA-IOS-EXIT=$rc
