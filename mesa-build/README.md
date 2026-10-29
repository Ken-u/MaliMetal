# PanVK 的构建（Linux 构建机，不需要 root）

全部产物在 `$MESA_BUILD/`（约 5 GB，大头是 `build/llvm`；用完可以删 `build/` 和 `dl/`，`prefix/` 是装好的工具）。
脚本按顺序跑，都是 `setsid nohup` 后台友好的：

| 顺序 | 脚本 | 做什么 | 耗时（40 核） |
| --- | --- | --- | --- |
| 0 | `python -m venv venv` + `pip install meson mako pyyaml packaging ply` | meson 1.12、mako（用 UniXNU 工具链的 python3.12 建 venv，不改它） | 十几秒 |
| 1 | `fetch.sh` | libclc / SPIRV-LLVM-Translator / SPIRV-Headers / SPIRV-Tools / glslang 源码（代理会掐 TLS，脚本里带重试） | 2 分钟 |
| 2 | `build-llvm.sh` | 从源码编 LLVM+Clang **20.1.8**（host x86，gcc 9.4）。**不能用 AOSP 预编译的 clang**：它没有 `opt`，而且 libLLVM 是 libc++ 静态打进去的，和 Mesa 的 C++ 混不到一起 | ≈ 14 分钟 |
| 3 | `build-spirv-glslang.sh` | SPIRV-Tools + glslang | ≈ 5 分钟 |
| 4 | `build-clc.sh` | SPIRV-LLVM-Translator（`llvm-spirv`）+ libclc（`spirv-mesa3d-`/`spirv64-mesa3d-`） | 1 分钟 |
| 5 | `build-libdrm.sh` | libdrm 2.4.123，只给 host 版 PanVK 用 | 十几秒 |
| 6 | `build-mesa-host.sh` | host x86 的 PanVK（`libvulkan_panfrost.so`）+ 装出 `mesa_clc`、`vtn_bindgen2`、`panfrost_compile` | ≈ 70 秒 |
| 7 | `build-mesa-ios.sh` | **交叉编译成 arm64-apple-ios17.0**（iPhoneOS16.5 SDK + AOSP clang 的 `clang-20` + `ld64.lld`），用第 6 步的 host 工具 | ≈ 1 分钟 |

Mesa 源码用的是 AOSP 树（`external/mesa3d`，26.1-devel）的拷贝：`prepare-mesa-src.sh` 把 `$MESA_SRC_ORIG` 拷到 `$MESA_BUILD/src/mesa/`，
再按顺序打 `../mesa-patches/0001…0008`（已在干净的 AOSP 拷贝上验证 8 个都能打上）。原树不改。

## 第 7 步踩到的坑（都已经在脚本里处理了）

1. **环境里的 `CCC_OVERRIDE_OPTIONS`**（UniXNU 内核构建塞的 `+-Wno-unused-but-set-global`）会让 clang 报 unknown warning，meson 的所有编译探测因此全部失败（`function attribute … : NO`、`PACKED` 没了，struct 布局全错）。脚本里 `unset`。
2. **AOSP 的 `bin/clang` 是个包装器**，直接用 `bin/clang-20`（C++ 用 `--driver-mode=g++`）。
3. **不能带 host 的 `PKG_CONFIG_PATH`**，否则 meson 会找到 host 的 x86 libdrm。
4. **`-Wl,-undefined,dynamic_lookup` 不能放进 cross file 或 `meson configure`**：meson 的 `has_function` 探测会全部变成「成功」（`secure_getenv`、`sched_getaffinity`、`__builtin_*_overflow_p` 全被误判存在，然后编译报错）。只在 `ninja` 阶段用 `CCC_OVERRIDE_OPTIONS='# +-Wl,-undefined,dynamic_lookup'` 追加。它是给 PanVK 的弱入口点（`panvk_*`，Mach-O 没有 weak alias）用的：链接后是 flat-namespace + `weak_import`，运行时找不到就是 NULL，跟 ELF 上的弱符号语义一致。
5. **clang 20 的 `__has_builtin(__builtin_add_overflow_p)` 返回真**，真用就报 unknown builtin；cross file 里 `-UHAVE___BUILTIN_ADD_OVERFLOW_P -UHAVE___BUILTIN_SUB_OVERFLOW_P` 让它走 Mesa 的纯 C 后备。
6. 环境里 `LLVM_CONFIG` 指向 UniXNU 工具链的替身，host 构建要 `export LLVM_CONFIG=$PREFIX/bin/llvm-config`。

## 现状

`build/mesa-ios/src/panfrost/vulkan/libvulkan_panfrost.dylib`：Mach-O arm64，17 MB，导出 `vk_icdGetInstanceProcAddr`、
`vk_icdGetPhysicalDeviceProcAddr`、`vk_icdNegotiateLoaderICDInterfaceVersion`。链接到的 libdrm 符号由 `../darwin-drm/` 的垫片提供——
**目前垫片是骨架**（`drmIoctl` 返回 ENOSYS，syncobj 全是桩），所以这个 dylib 还不能干活，但「PanVK 能不能在 iOS 上编出来」这个问题有答案了：能。
