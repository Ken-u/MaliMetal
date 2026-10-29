#!/bin/bash
# 在构建机上跑。把 Metal 插件（NativeMaliMetalPlugin.mm + 原生 Metal 前端 + Rust 桥）编成 NativeMaliMetal.bundle（ad hoc 签名，4 KiB 页），
# PanVK（libvulkan_panfrost.dylib）放在 bundle 里，和 Adreno 的做法一样（Turnip 在 NativeAdrenoMetal.bundle 里）。
#   out/NativeMaliMetal.bundle/{NativeMaliMetal, Info.plist, libvulkan_panfrost.dylib}
# 桥库先用 build-bridge.sh 编出来（$BRIDGE）。
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../env.sh"
ROOT="$MALIMETAL_ROOT"
DRIVER="$MALI_DIR"
SDK="${SDK:-${IOS_SDK:?set IOS_SDK in .env}}"
A="${LLVM_BIN:?set LLVM_BIN in .env}"
MESA="${MESA:-${MESA_BUILD:?set MESA_BUILD in .env}}"
ICD="${ICD:-$MESA/build/mesa-ios/src/panfrost/vulkan/libvulkan_panfrost.dylib}"
BRIDGE="${BRIDGE:-${BRIDGE_OUT:?set BRIDGE_OUT in .env}/target/aarch64-apple-ios/release/libnative_metal_bridge.a}"
PY=/usr/bin/python3
OUT="$HERE/out"
BUNDLE="$OUT/NativeMaliMetal.bundle"
unset CCC_OVERRIDE_OPTIONS LLVM_CONFIG CLANG_BIN CC CXX
[ -f "$BRIDGE" ] || { echo "error: 没有桥库 $BRIDGE（先跑 build-bridge.sh）" >&2; exit 1; }
[ -f "$ICD" ] || { echo "error: 没有 ICD $ICD" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$BUNDLE"

"$A/clang-20" -target arm64-apple-ios17.0 -isysroot "$SDK" -O2 -g0 -std=c++17 -fobjc-arc -Wall -Wextra -Wno-deprecated-declarations \
	-Wno-unused-parameter -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable \
	-I"$HERE/frontend" -I"$DRIVER" -bundle -fuse-ld=lld -B"$A" -Wl,-adhoc_codesign -Wl,-dead_strip \
	-x objective-c++ "$HERE/NativeMaliMetalPlugin.mm" "$HERE/frontend/NativeBridgeMetalFrontend.mm" -x none "$BRIDGE" \
	-lobjc -lc++ -framework Foundation -framework Metal -framework IOSurface -o "$BUNDLE/NativeMaliMetal" \
	2> >(grep -v "directory not found" >&2) || { echo "error: 插件编译/链接失败" >&2; exit 1; }
cp "$ICD" "$BUNDLE/libvulkan_panfrost.dylib"
"$PY" - "$BUNDLE/Info.plist" <<'PYEOF'
import plistlib, sys
open(sys.argv[1], 'wb').write(plistlib.dumps(dict(
    CFBundleIdentifier='io.github.ken-u.NativeMaliMetal', CFBundleExecutable='NativeMaliMetal', CFBundlePackageType='BNDL',
    CFBundleVersion='1', CFBundleShortVersionString='1', NSPrincipalClass='NativeMaliMetalDevice',
    MinimumOSVersion='17.0'), fmt=plistlib.FMT_XML))
PYEOF
ls -l "$BUNDLE"
"$PY" "$ROOT/usertest/cdhash.py" "$BUNDLE/NativeMaliMetal"
"$PY" "$ROOT/usertest/cdhash.py" "$BUNDLE/libvulkan_panfrost.dylib"
"$PY" "$HERE/make-addon.py" "$OUT/NativeMaliMetal.addon"
