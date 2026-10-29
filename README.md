# MaliMetal

The user-space graphics stack for the Mali-G610 on UniXNU/RK3588 (EVB10): Mesa **PanVK** (Vulkan)
cross-built for `arm64-apple-ios`, a small **libdrm stand-in** that turns Panthor ioctls into calls
on the [Mali](https://github.com/Ken-u/Mali) IOKit user client, and the **Metal plug-in** that lets
the stock iPadOS 17 Metal clients (backboardd, SpringBoard, QuartzCore) run on top of Vulkan through
`metal2vulkan`.

```
backboardd -> AGXAccelerator -> NativeMaliMetal plug-in -> metal2vulkan bridge (Rust) -> PanVK
           -> darwin-drm (libdrm stand-in) -> Mali user client (kernel) -> Mali-G610 CSF
```

| Path | Contents |
| --- | --- |
| `darwin-drm/` | The libdrm stand-in: DRM ioctl numbers and structures, `drmIoctl`, syncobj and mmap-offset emulation on top of `mali_uapi.h` |
| `mesa-patches/` | Eight patches on Mesa 26.1-devel (PanVK for Darwin, host-pointer import, external buffers, rasterization-order access, no host copy for compressed formats, masked render targets that are not written) |
| `mesa-build/` | Scripts that build LLVM 20, SPIRV tools, host Mesa tools and finally `libvulkan_panfrost.dylib` for iOS; `README.md` explains the order |
| `metal-plugin/` | `NativeMaliMetalPlugin.mm`, the Metal front end, the Rust bridge, a metal2vulkan patch, `make-addon.py` that packs the plug-in into the kernel's GPU add-on |
| `usertest/` | Stand-alone iOS test programs (compute, render, Vulkan triangle) that the kernel can run as the init process (`evb10-exec=1`) |
| `docs/notes.zh-CN.md` | Development notes in Chinese: findings, measurements, dead ends |

## Building

Everything runs on a Linux build host without root. Copy `.env.example` to `.env` and fill in the
tool and source locations (the file is git-ignored; all scripts read it through `env.sh`).

```sh
mesa-build/fetch.sh && mesa-build/build-llvm.sh && mesa-build/build-clc.sh
mesa-build/build-spirv-glslang.sh && mesa-build/build-libdrm.sh
mesa-build/prepare-mesa-src.sh                # copy Mesa and apply mesa-patches/
mesa-build/build-mesa-host.sh && mesa-build/build-mesa-ios.sh
metal-plugin/m2v-patches/apply.sh && metal-plugin/build-bridge.sh
metal-plugin/build-plugin.sh && metal-plugin/make-addon.py ...
usertest/build.sh                             # optional on-board test programs
```

Inputs that are not in this repository: the Mesa tree (`MESA_SRC_ORIG`), the iPhoneOS 16.5 SDK
(`IOS_SDK`), a clang toolchain (`LLVM_BIN`, `LLVM_BIN_NEW`) and the pinned metal2vulkan checkout
(`M2V`, revision `720ef1ce`, plus the patches from the UniXNU `AdrenoMetal` driver at `d6ca655`).

## Status

This is a development snapshot. Verified on the EVB10 board (iPadOS 17.7.11): the lock screen is
composited on the GPU. While scrolling the home screen the panel gets about 50 frames per second in
the steady state (the best swipe reaches 58) with the boot-arguments of the frame-rate runs
(`docs/notes.zh-CN.md`, "帧率": zero-copy scanout with the GPU fence, the GPU at 750 MHz, the device's
YCbCr filter, the A76 cores numbered first); without them the board still scrolls at the older, much
lower rate. 60 fps is not reached: the heavy frames at the start of a swipe miss their deadline by
a few milliseconds, most of it the render server's encoding thread. The GPU and CPU clocks are fixed.
Measurements and the open items are in `docs/notes.zh-CN.md`.

## License

BSD-2-Clause-Patent for the original code (see `LICENSE`). Mesa patches are MIT like Mesa itself,
`metal-plugin/frontend/` and `metal-plugin/bridge/` derive from NativeMetal (MIT, see
`metal-plugin/frontend/LICENSE`), and the metal2vulkan patch follows that project's license.
