#!/bin/bash
# 在构建机上跑。把 NativeMetal 的 Rust 桥（metal2vulkan 运行时 + NativeMetalBridge.rs）交叉编成 aarch64-apple-ios 的静态库。
# 对照 UniXNU/scripts/build-native-metal-bridge.py（那份用 xcrun/brew，只能在 macOS 上跑）：流程一样，工具换成 Linux 上有的。
#   - metal2vulkan 源码在 $M2V（steelbrain/metal2vulkan @ 720ef1ce，已打上上游 AdrenoMetal/bridge/patches 的 0001-0037 + 本板 8 MiB 池块：m2v-patches/apply.sh）
#   - llvm-as 用 AOSP 的 clang-r596125（夹具 .ll 需要 LLVM 21+）
#   - cargo +1.98.0（rustup 装在 ~/.rustup，带 aarch64-apple-ios 的 std）
#   - sha2 的 asm 特性要一个 C 编译器（cc crate）：clang + iPhoneOS16.5 SDK
# 产物：$OUT/libnative_metal_bridge.a
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../env.sh"
M2V="${M2V:?set M2V in .env}"   # 钉住的 720ef1ce + 上游 0001-0037 + 本板 8 MiB 池块（m2v-patches/apply.sh）
SDK="${SDK:-${IOS_SDK:?set IOS_SDK in .env}}"
LLVM="${LLVM:-${LLVM_BIN_NEW:?set LLVM_BIN_NEW in .env}}"
OUT="${OUT:-${BRIDGE_OUT:?set BRIDGE_OUT in .env}}"
BRIDGE_RS="${BRIDGE_RS:-$HERE/bridge/NativeMetalBridge.rs}"
mkdir -p "$OUT"
unset CCC_OVERRIDE_OPTIONS
"$LLVM/llvm-as" -o "$OUT/runtime_examples.air" "$M2V/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll"
"$LLVM/llvm-as" -o "$OUT/function_constant.air" "$M2V/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll"
# GPU 扫描输出的半浮点转换核（手写 AIR；上游 d6ca655 起桥库把它 include_bytes! 进来）
"$LLVM/llvm-as" -o "$OUT/scanout_convert.air" "$HERE/bridge/scanout_convert.ll"
# 运行时标识：译码器源码（钉住的版本 + 补丁后的内容）的哈希，着色器缓存按它分键（上游同样做法）
RUNTIME_ID="$( (git -C "$M2V" rev-parse HEAD; cd "$M2V" && find crates Cargo.toml -type f -not -path '*/target/*' | sort | xargs sha256sum) | sha256sum | cut -c1-32)"
# release 配置同上游（fat LTO + 单代码生成单元：显示线程大半时间在译码器运行时的小函数里，跨 crate 内联）；PROFILE_EXTRA= 空串可关掉（快编）
if [ -z "${PROFILE_EXTRA+x}" ]; then PROFILE_EXTRA=$'lto = "fat"\ncodegen-units = 1'; fi
cat > "$OUT/Cargo.toml" <<TOML
[package]
name = "native-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = "$BRIDGE_RS"
[dependencies]
metal2vulkan = { path = "$M2V/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$M2V/crates/metal2vulkan-runtime" }
libc = "0.2"
sha2 = { version = "0.10", features = ["asm"] }
[profile.release]
panic = "unwind"
debug = 1
${PROFILE_EXTRA}
TOML
export NMB_FIXTURE_AIR="$OUT/runtime_examples.air" NMB_FC_AIR="$OUT/function_constant.air" NMB_SCANOUT_AIR="$OUT/scanout_convert.air" NMB_RUNTIME_ID="$RUNTIME_ID"
export IPHONEOS_DEPLOYMENT_TARGET=17.0 SDKROOT="$SDK"
export CC_aarch64_apple_ios="$LLVM/clang" AR_aarch64_apple_ios="$LLVM/llvm-ar"
export CFLAGS_aarch64_apple_ios="-target arm64-apple-ios17.0 -isysroot $SDK"
export CARGO_TARGET_AARCH64_APPLE_IOS_LINKER="$LLVM/clang"
cd "$OUT"
cargo +1.98.0 build --release --manifest-path "$OUT/Cargo.toml" --target aarch64-apple-ios --target-dir "$OUT/target" 2>&1 | tail -40
ls -l "$OUT/target/aarch64-apple-ios/release/libnative_metal_bridge.a"
