# MaliMetal 开发笔记（中文，开发日志）

> PanVK / metal2vulkan / 插件部分的实测记录。命令里的路径用环境变量表示；`板子运行 X` 指当时保存的串口日志。内核驱动部分见 Mali 仓库的 `docs/notes.zh-CN.md`。

## M3-1：PanVK（G610）够不够 metal2vulkan 用——纸面结论：够（2026-10-28，没碰板子）

对照物：metal2vulkan @ `720ef1ce`（UniXNU 钉的那版）的 `profiles/vk12-floor.json`（消费者永远要的下限）
和 `device/profile.rs`；PanVK = AOSP 树 Mesa 26.1-devel（`875160ed`）的 `panvk_vX_physical_device.c`，PAN_ARCH=10。

| floor 要求 | PanVK v10 |
| --- | --- |
| Vulkan 1.0/1.1/1.2 必选：`robustBufferAccess`、`multiview`、`hostQueryReset`、`imagelessFramebuffer`、`separateDepthStencilLayouts`、`shaderSubgroupExtendedTypes`、`subgroupBroadcastDynamicId`、`timelineSemaphore`、`uniformBufferStandardLayout` | **全部 true** |
| 消费者下限：`shaderInt16`、`shaderStorageImageRead/WriteWithoutFormat`、`storageBuffer16BitAccess`、`storageBuffer8BitAccess` | **全部 true** |
| 另外 | 对外宣称 **Vulkan 1.4**（conformance 1.4.1.2）；`shaderInt64`、`bufferDeviceAddress`、`descriptorIndexing`（arch≥9）、`runtimeDescriptorArray`、`shaderFloat16`（arch≥10）、`shaderInt8`、`nullDescriptor`、`KHR_push_descriptor`、`KHR_dynamic_rendering_local_read`、`KHR_synchronization2`、`KHR_maintenance1..9` 都有 |

**PanVK 没有的**（metal2vulkan 对它们是「按 pipeline 给类型化拒绝」，不是全局门槛，UI 合成基本不碰）：
`geometryShader`、`tessellationShader`、`shaderFloat64`、`fillModeNonSolid`、`shaderClipDistance/CullDistance`、
`shaderOutputLayer`、像素/采样 interlock，以及 **`VK_EXT_rasterization_order_attachment_access`**。
最后一条要留意：`policy.rs` 默认 `framebuffer_fetch = RequireOrdered`，没有这个扩展时 framebuffer-fetch 的 pipeline 会被拒；
PanVK 有 `KHR_dynamic_rendering_local_read`，G610 是 TBDR（同一像素在 tile 里天然有序），所以两条出路：
消费者改用 `AcceptUnordered`，或给 PanVK 补一个只宣称不改行为的 EXT。**UniXNU 的 NativeMetal 用哪个策略要看它的源码**
（本地 submodule 没检出），上板前先确认。

没有验证的部分：这是静态读源码，不是在 G610 上跑 `vulkaninfo`；性能（着色器编译耗时，Turnip 那边 58 个 pipeline 花过 68 s，
后来用 release 构建才降下来）要等 PanVK 真的能跑才知道。

## M3-2：架构决定——仿 Panthor uAPI，Mesa 基本不动

PanVK 的 CSF 路径**不只**走 `pan_kmod` 抽象：`panvk_vX_gpu_queue.c`、`panvk_vX_bind_queue.c`、`panvk_sparse.c`、
`panvk_vX_utrace.c` 都直接发 Panthor ioctl，同步对象用的是 `vk_drm_syncobj`。所以照 Turnip 那样「只加一个 kmod 后端」不够。
更省事的做法：**在用户态做一层 DRM 垫片**，把下面这些 ioctl 转成 IOKit user client 调用，PanVK / `panthor_kmod.c` 一行不改。
实际用到的清单（`rg DRM_IOCTL` 数出来的，共约 25 条）：

| 类别 | ioctl |
| --- | --- |
| Panthor | `DEV_QUERY`、`VM_CREATE/DESTROY/BIND/GET_STATE`、`BO_CREATE/MMAP_OFFSET/SYNC/SET_LABEL/QUERY_INFO`、`GROUP_CREATE/DESTROY/GET_STATE/SUBMIT`、`TILER_HEAP_CREATE/DESTROY`、`SET_USER_MMIO_OFFSET`（要映射一页「用户 MMIO」，里面是 LATEST_FLUSH_ID） |
| DRM syncobj | `CREATE/DESTROY/RESET/WAIT/SIGNAL/TRANSFER/QUERY/FD_TO_HANDLE/HANDLE_TO_FD` |
| 其它 | `drmGetDevices2` 之类的设备枚举、`mmap` 偏移约定、dma-buf/prime（Darwin 上换成 IOSurface/共享内存句柄） |

内核侧（Mali）要实现的就是这些 ioctl 的语义：VM（每个 context 一个 AS，`vmMap/vmUnmap`）、BO、group/queue
（`m2()` 已经证明了启动序列）、syncobj（内核里一个 u64 时间线 + 等待队列）、tiler heap、中断驱动的完成通知。

## M3-3：PanVK 能在 构建机 上交叉编成 arm64-iOS 了（2026-10-28，已验证）

Turnip 的构建是 `-Dllvm=disabled`；PanVK 的 `libpan`（预编译内部着色器）要 **`mesa_clc` + `vtn_bindgen2`（C 程序）+ libclc +
SPIRV-LLVM-Translator + SPIRV-Tools + glslang**，host 上要有 LLVM 开发环境。预想的「最大未知项」其实一个下午就打通了：

| 步骤 | 结果 |
| --- | --- |
| 私有前缀里从源码编 LLVM/Clang 20.1.8、SPIRV-Tools、glslang、`llvm-spirv`、libclc、libdrm（全部 `$MESA_BUILD/prefix`，不碰系统） | 总共 ≈ 20 分钟（40 核）。AOSP 预编译 clang **不能用**（没有 `opt`，libLLVM 带静态 libc++） |
| host x86 版 PanVK（`libvulkan_panfrost.so`）+ 装出 `mesa_clc` / `vtn_bindgen2` / `panfrost_compile` | 70 秒，一次过 |
| **arm64-apple-ios17.0 交叉编译** `libvulkan_panfrost.dylib` | **链接成功**，17 MB，导出 `vk_icdGetInstanceProcAddr` 等 |

让它链过去要的全部改动（比 Turnip 的 1400 行小得多）：

1. Mesa 补丁 `mesa-patches/0001-panvk-darwin-build.patch`（4 个文件、91 行）：meson 在 darwin + panfrost-vk 时启用 libdrm 依赖；
   `panvk_physical_device.c` 里 `open(render_node)` 换成 `drm_darwin_open_render()`、跳过 `stat(st_rdev)`；`vk_image.[ch]` 的 `drm_format_mod` 条件加上 `DETECT_OS_APPLE`。
2. `MaliMetal 的 darwin-drm/`：libdrm 垫片（`xf86drm.h` + `drm_darwin.c` + `sys/sysinfo.h` + `sys/sysmacros.h` + `libdrm.pc.in`），
   PanVK 实际用到的只有 ~27 个符号。**目前是骨架**：`drmIoctl` 返回 ENOSYS，syncobj 是桩，设备枚举返回一台假的 platform 设备，`drmGetVersion` 报 `panthor`。
3. 构建脚本 `MaliMetal 的 mesa-build/`（README 里有顺序、耗时和 6 个坑——最阴的是环境里的 `CCC_OVERRIDE_OPTIONS` 会让 meson 所有编译探测失败）。

弱入口点（Mach-O 没有 weak alias）：链接用 `-undefined dynamic_lookup`，结果是 flat-namespace + `weak_import`，运行时没有实现就是 NULL，
和 ELF 弱符号一致，Mesa 的 dispatch 表会回落到 `vk_common_*`。已经用 `llvm-objdump --macho --chained-fixups` 确认。

## 客户端进程：poster 扩展等自己要 Metal 设备（gpu43…gpu54）

backboardd 之外，锁屏壁纸由 poster 扩展进程自己画（`AegirPoster`、`KaleidoscopePoster`、`CollectionsPoster`、`PhotosPosterProvider`、`MercuryPosterExtension`、
`GradientPosterExtension`、`EmojiPosterExtension`、`ExtragalacticPoster`、`HelloPosterExtension`、`PridePosterExtension`、`UnityPosterExtension`）。它们各自 `dlopen`
Metal → 插件 → PanVK，**每个进程一个独立的 Mali 连接**：

- **放行名单有两处**：内核 `Mali.cpp` 的 `NativeSandboxGrantProcess`（服务 `mali-grant`、加速器 `mali-plugin-grant`，代码里有默认名单，boot-arg 可整体覆盖；
  内核按 `proc_selfname` 的前 16 字符比，所以是 `CollectionsPoste`、`PhotosPosterProv`…）；插件 `clientProcess()` 用 `getprogname()`（全名）。两边都要有，缺一边就是「没设备」。
  还没放行的 Metal 客户端：PosterBoard、WidgetRenderer_D、SpringBoard（bundle 加载了但拿不到设备）、mediaserverd——目前不影响出画面。
- **一个客户端 = 1 个 VM（页表）+ 2 个 group（各 3 个 queue）+ 2 个 tiler heap**。Mali 只有 8 个地址空间（AS0 给 MCU）和 8 个 CSG 槽，所以内核里做了**虚拟化**
  （gpu51 `no free group slot` / `device refused -3` 之后）：
  VM 和 group 是句柄（`gCtx[]` / `gGroup[]`），不再等于硬件资源；`ctxActivate()` 给 VM 找 AS（LRU，驱逐没有驻留 group 的 VM），`groupActivate()` 在 `GROUP_SUBMIT` 时给 group 找槽
  （驱逐空闲的 LRU group：`groupIdle` = 所有 syncobj 到了 seqno 且环读空；`groupStop` 终止 CSG 再释放；`groupStart` 按环里已有的 insert/extract 续上）。
  全占满且都有活时 `GROUP_SUBMIT` 返回 `kIOReturnNotReady`，`externalMethod` 释放锁睡 1 ms 重试（最多 1 万次）——用户态（`drm_darwin.c`）遇到错误不重试，所以等待只能在内核里。
- **新开设备很贵**：每个连接建 VM、2 个 group、2 个 tiler heap（5×2 MiB 块）、环和 syncobj，加载 Mesa dylib。dyld 的 flat-namespace weak import 曾让库加载花 1 秒以上
  （`mesa-build/fix-weak-imports.py` 把 weak 标记去掉，现在 5–14 ms）。metal2vulkan 的设备内存池块 64 → 8 MiB（`BLOCK_BYTES`；大于半块的请求单独成块）——
  每个 BO 都是物理连续、清零、映射（64 MiB ≈ 70 ms）。
- **扩展被 RunningBoard 杀：`reason 7/0x3 extension check-in timeout`**：从启动算约 4.7–5 s 内必须 check-in。设备要在 check-in 之前建好（poster 在 `+[PRPosterProvider …]` 里同步建 Metal），
  所以任何一步慢都会被杀、PosterBoard 再拉起、再杀（每 5 s 一圈）。内核 `evb10: exit pid N (名字) … reason 7/0x3` 能看到。
- **内存**：扩展有 200 MB 的 jetsam 上限（`reason 1/0x2`），`CollectionsPoster` 的高水位 15 MB（`EXC_RESOURCE`）。AegirPoster 一启动要约 446 MB GPU 缓冲（45 个 8 MiB 块 + 一块 152 MiB），
  超限；连接关闭时内核打 `closes its connection: N buffers made, M MB of them alive at the end`。
- **Data 里有状态**：全新 Data（`emmc-data-reset=1`，**只跑一轮，之后别带**）时 PosterBoard 同时拉起所有 poster 扩展（约 196–289 s 之间）；Data 里有了状态之后每次开机仍会在
  ≈288 s 拉起一批。活动的默认 poster 是 Collections / Photos。

### 这一段的观测手段

- `evb10-ps=8`：每 ~10 s 打进程表；`evb10-bt=<进程名>`（≤16 字符，另有 `bt2`/`bt3`/`bt4`）：每线程用户态 FP 链 `ubt …` + 同一行的 `slide`；翻译
  `ULAND=/tmp/uland /usr/bin/python3 uefi-evb10 的 tools/xnu-probe/symbolize/dsc.py sym --slide HEX ADDR…`（0x10… 开头的地址是我们自己的镜像——ICD / 插件 / 进程本体——不在 dyld 缓存里，要自己按镜像基址算）。
  `evb10-kbt=1`：采样器顺带走内核栈（**默认关**：它和被采线程竞争，gpu52 在 `machine_trace_thread64` 里 data abort 过）。
- `mali: slow call <进程> sel N: X ms, waited Y ms for the lock`、每 10 s 的 `calls (selector:count total/lock-wait/max)`；`makes buffer N of K KiB`、`closes its connection …`（逐客户端的 BO 账）。
- 故障：`gpuFaultScan()` 只在 AS 故障状态寄存器非零时报 MMU 故障（`MMU_IRQ_RAWSTAT` 的位 16+as 是命令完成，不是总线错误，曾经把 80 条额度全吃光）；CS `fault`/`fatal` 照报。
- `STALL group N in slot S …`（`gpuStallScan()`）：有作业 2 秒还没完成的驻留 group，打 CSG/CS 的 req/ack、环 extract/insert、CS 状态、MMU AS 状态，每秒最多扫一次、在任何一次外部调用里。
  **用户态 `nap()`（`drm_darwin.c`）是自己在轮询 syncobj**，内核看不到这类等待，所以才要内核自己扫。
- `mali-skipslot=<掩码>`：不往这些 CSG 槽里放 group（实验用）。

## 上游更新：UniXNU `feat/ipados27-gpu-parity`（2026-10-29 合进来的）

用户提醒官方代码有更新、框架有优化。核对结果：

- 官方 `main` 比我们本地树（`f85b5a6`）只多 6 个**文档**提交，代码没变；真正的优化在分支 **`feat/ipados27-gpu-parity`**（35 个提交，当天还在更新），在 `drivers/AdrenoMetal`
  （Metal 前端 + Rust 桥 + 译码器补丁）、`drivers/GopFramebuffer`、`drivers/Adreno740` 等子模块里。我们**没有**切 UniXNU 的工作树（它有本地修改，而且内核补丁系列要对着 main），
  只 `git fetch` 之后用 `git show` 取出文件。AdrenoMetal 在 `d6ca655`。
- **我们的 Metal 前端是 `fbd282e`（2026-10-22）的拷贝加 117 行本地改动**（console 转发、XR 窄化、sys_dcache_flush 覆盖、probeSurface、dedicatedMemorySize、答案缓存）；
  Rust 桥源（`NativeMetalBridge.rs`、`NativeMetalObjects.rs`、两个 `.h`）和基线一字不差。三方合并（`git merge-file`，base = fbd282e）：10 处冲突，全部手工解决——
  我们的答案缓存和上游同时各写了一份（上游 `askAnswer` 同一个思路，留上游的）；`narrowXR` 与上游的 PVRTC 解码并存；其余取上游。结果 4329 行（原来 1794）。
  SDK 是 iPhoneOS 16.5 的，缺两个名字：`MTLBindingAccess`（= `MTLArgumentAccess`）、`MTLLanguageVersion3_1`，在前端文件顶部补了 typedef。
- **译码器（metal2vulkan，钉在 `720ef1ce`）：上游现在是 37 个补丁**（我们只打了 5 个）：绘制路径少分配（`keep-bind-vectors`、`keep-draw-commands`、`skip-unchanged-rebinds`、`key-hash-the-draw-path-maps`、
  `reset-command-pools-as-given-back`、`calibrate-off-the-commit-path`…）、增长的内存池块（`growing-memory-blocks`：从 4 MiB 起倍增）、着色器缓存目录可选、Core Image 内核库等。
  `MaliMetal 的 metal-plugin/m2v-patches/apply.sh` 在一个干净克隆（`$GPU_M4/metal2vulkan-up`，`git clone --shared` 后 checkout 720ef1ce）上按上游顺序打 0001–0037，
  再加我们一处改动：**`BLOCK_BYTES` 8 MiB**（上游 64 MiB；我们的内核对每个 BO 做物理连续+清零+映射，扩展有 200 MB jetsam 上限）。
- 桥的构建（`build-bridge.sh`）跟着变：`NMB_SCANOUT_AIR`（`bridge/scanout_convert.ll`，`llvm-as` 出来 `include_bytes!`）、`NMB_RUNTIME_ID`（译码器源码哈希，缓存分键）、`SDKROOT`（cc-rs 在新目录里会找 `xcrun`）。
  `PROFILE_EXTRA=$'lto = "fat"\ncodegen-units = 1'` 就是上游的 release 配置（4 分 43 秒，静态库 309 MB → 191 MB）。
- **着色器缓存终于跨启动了**：上游发现 `/var/mobile/tmp` 每次开机被 launchd 的 `dirs_cleaner` 清空，缓存目录换成 `~mobile/Library/Caches/com.apple.<进程名>/m2v`（我们的插件先试这个）。
  实测：第一次启动 `cache hit 0 miss 58 / total=919ms`（backboardd），第二次 `cache hit 26 miss 0 / total=57ms`，整机 77 条管线从 2340 ms 降到 149 ms。
  还加了「没有可写缓存目录就带 `METAL2VULKAN_SHADER_CACHE_OPTIONAL=1` 无缓存打开」的回退，以及 `first problem:` 行（进程的第一条问题，在命中它的线程上打出，崩溃前也能看到）。
- **效果（`up5`，`evb10-shot-img=1` 截图）：锁屏壁纸出来了**——iPadOS 17 默认的彩色波浪壁纸完整显示（之前一直是黑的；更新前后唯一的区别就是前端/译码器补丁，多半是
  `tile-stores-within-rows`、`mipmaps-outside-the-measured-spread`、`two-plane-shadow-as-memory-lies` 之一，没有逐个二分）。**时钟那一块还是坏的**：时间文字所在位置有一个黑色矩形，
  里面只剩几块白色碎片，日期被它切掉一半（矩形约 520×180 px，在整屏 1072×1904 里）——那一层多半是材质/玻璃（背景采样 + 混合）的 pass 在采样一个全黑的纹理。下一步看这个 pass 的管线和输入。
- 译码器 LTO 版（`up4`）与普通版（`up3`）在这些场景下没有可见差别（GPU 是瓶颈）；管线编译总耗时 345 ms → 149 ms（最长一条 179 ms → 3 ms，那是缓存命中率的差别，不是 LTO）。
  `build-bridge.sh` / `build-plugin.sh` 的默认值现在是 LTO 版（上游的 release 配置），`PROFILE_EXTRA=` 设空可以快编。
- **没有搬**：`METAL2VULKAN_QUEUE_PRIORITY=high`（对应的补丁不在上游的 37 个里）；`PrimeShaders.h`（开机 90 s 后把系统的 metallib 全过一遍译码器填缓存，要改很多，先看缓存命中率够不够）；
  `GopFramebuffer` 的「保留每个 swap 面的客户端映射」（我们的零拷贝路径本来就按槽缓存映射）、`Adreno740`/`ArmSmmu4K`/`XnuPrelink` 等（Adreno 专有，或和我们的内核补丁系列无关）。

## 时钟黑矩形：PanVK 把「被屏蔽的颜色附件」算成「写了」（2026-10-29，`板子运行 clk1…`clk7`）

**症状**：锁屏壁纸出来之后，时间文字位置是一块黑矩形（约 520×180 px），里面只剩白色碎片，日期被切掉一半。

**根因**（Mesa patch `mesa-patches/0008-panvk-masked-rt-not-written.patch`）：
- CA 的时钟 pass（backboardd 的第 3 个 pass）是这样的：先用一个 `Tatc`（attachment-clear 四边形）清附件；然后把字形画进**无内存（memoryless）的 M1**
  （绘制写 color attachment 1，attachment 0 的写掩码为 0）；最后几次合成绘制（`Isrc_IsrcM1`、`Idst` 背景拷贝、`Tvcm…IsrcM1` 振动材质）写 attachment 0。
- metal2vulkan 把 Metal 的 framebuffer fetch 翻成「每个颜色附件同时也是输入附件」，memoryless 附件落在一块真的替身图像上（`attachment_stand_in.rs`）。
- PanVK（`csf/panvk_vX_cmd_draw.c: prepare_dcd`）的 `render_target_mask` / `write_mask` 取自 `color_attachment_written_mask(fs, …)`——只看**片元着色器写了哪些输出**，
  **不看** `VkColorBlendAttachmentState.colorWriteMask` / `colorWriteEnable`。于是「着色器写了 attachment 0，但写掩码是 0」的绘制，在 DCD 里仍把 attachment 0 当成被写；
  **一个 32×32 的 tile 只要被这样的绘制完全盖住，Mali 就认为这个 tile 的 attachment 0 已被覆盖，FPK/提前杀像素生效，tile 存回的是没被真正写过的值（黑）**。
  只盖住一部分的 tile 没事，所以黑块边缘是 tile 对齐的，再叠上斜向的字形包围盒形成「矩形里有碎片」。
- 修法：新增 `color_attachment_effective_written_mask()`——着色器写了的输出里，把 `color_write_enables` 关掉或 `attachments[i].write_mask == 0` 的位去掉；
  `rt_written` 用它；`dcd1_dirty` / `dcd2_dirty` 也跟着 `CB_WRITE_MASKS` / `CB_COLOR_WRITE_ENABLES` 的变化重算。
- 板上结果（`clk6`、`clk7`）：日期和 "4:03" 都对，整屏（壁纸 + 日期 + 时钟 + 底部横条 + 状态栏）和预期一致；没有 GPU 故障，没有进程被杀；
  栅栏统计正常（18 个 swap 全部是 GPU 作业没做完时到达，22 次翻页里 10 次等待、平均 34 ms、0 次放弃）。

**排查过程里走过的弯路（避免重走）**
- H1「附件序号错位，清屏写进了 c0」：被反驳——清屏矩形上方的日期文字毫发无损，说明清屏没写到该写的附件之外。
- 把 color preload 模式强制 ALWAYS（环境变量 `PANVK_PRELOAD_ALWAYS`，`panvk_vX_cmd_frame_shaders.c: always_load()`，**仅诊断用**）没有改善——不是 preload 的问题。
- 决定性证据是 1:1 截图裁剪（`evb10-shot-scale=1 evb10-shot-y=160 evb10-shot-rows=210`）：黑块边缘严格按 32 对齐。看这种东西 4:1 缩略图看不出来。

**新增的诊断工具（只在 backboardd 里生效；插件 `NativeMaliMetalPlugin.mm: startDumpFromBootArgs()`）**

| boot-arg | 作用 |
| --- | --- |
| `mali-dump=FIRST,COUNT` | 转储第 FIRST 起 COUNT 个命令缓冲（`NATIVE_METAL_DUMP_CB`）：每个 pass/绘制/管线的摘要（`c0=f80m0 c1=f80b`：格式、`m0` = 写掩码 0、`b` = 混合；memoryless 附件）+ 读回的 32×8 亮度网格 |
| `mali-dump-draws=A,B` | 只转储第 A…B 个绘制 |
| `mali-dump-image=WHERE` | 只转储某个图像（`_` 代表空格） |
| `mali-dump-match=TEXT` | 只转储名字含 TEXT 的 |
| `mali-dump-words=N` | 每个绘制转储 N 个描述符字 |
| `mali-skip-draws=PASS,FIRST,LAST` | 跳过某 pass 的第 FIRST…LAST 个绘制（`NATIVE_METAL_SKIP_DRAWS`），做「去掉哪个绘制黑块就没了」的二分 |

输出在内核串口里是 `native-metal dump cb N: …`。

**这套工具的坑**
- **backboardd 的沙盒会直接杀进程**（退出 `reason 25/0xdd`，`OS_REASON_SANDBOX`）：`pipe()`、在 fd 2 上新建文件 / `dup2` 都会触发。
  转发 stderr 要用 `funopen()` 做一个自定义 `FILE*` 再赋给 `stderr`（见 `consoleWrite()`，180 字符一块 → `mlog`，带互斥）。
- **boot-args 的字符集和长度**：`UniXNU/scripts/unixnu/payload.py: check_args` 的值只允许 `[A-Za-z0-9_.,/\-]`（没有 `:`、空格），整条命令行 ≤1000 字符；
  默认 boot-args 已有约 960，要加诊断参数得先腾地方：`evb10-bt=x evb10-bt2=x evb10-bt3=x evb10-bt4=x`（省约 38 字符），必要时去掉 `evb10-vop-fence=1`。
  （2026-10-30 之后 `board.toml` 默认已经去掉 `evb10-ps` 和这四个 `evb10-bt*`，默认是 884 字节，见「帧率（二）」第 9 节。）
  逗号在插件里转成冒号，下划线转空格。
- 命令缓冲的编号每次运行不一样（锁屏帧的序号会漂），要用宽窗口（`mali-dump=1,40`）加 `mali-dump-draws=1,12`。
- 内核 printf 超过约 255 字符会丢尾巴，转储每行已经切到 180。
- 截图：`evb10-shot-img=1`（默认 4:1 RGB565）；`evb10-shot-scale=1|2|4`、`evb10-shot-y=Y0`、`evb10-shot-rows=N` 取一块 1:1 裁剪（PNG 只含这些行，绝对 y = Y0 + png y）；
  转 PNG 用 `uefi-evb10 的 tools/xnu-probe/shot2png.py <log> <输出目录>`。

## 滚动一帧要 100 ms：壁纸的 YCbCr 每个编码器都重新转换（2026-10-29，`板子运行 perf1…`perf8`）

**症状**：滚动（主屏翻页）时真实帧率 8–10 fps，`mali: user: status: K…` 的 `ctc`（提交到回调）100–380 ms；当时看到 GPU 时钟 198 与 594 MHz 帧时间一样，据此认为不是着色器算力。
**这条推断后来被推翻**（见下面「帧率」一节）：那是在 73–77 ms 的转换 dispatch 还占着一帧的时候量的；去掉转换之后，重通道的耗时严格随 GPU 时钟走（198 / 594 / 750 MHz：84.9 / 28.4 / 22.5 ms）。

**新增的量法**（都在插件里，只对 backboardd 生效，除非另说）：

| 东西 | 用法 / 含义 |
| --- | --- |
| K 行 `gpu=…us st=…us` | 一个命令缓冲的 GPU 耗时（两个时间戳：桥给每个批次开头结尾写的）和提交后多久才开始。`gpu=0us st=-1us` = 在途命令缓冲太多，没分到时间戳槽 |
| `mali-panvk-debug=…` | 设 `PANVK_DEBUG`（`noafbc`、`linear` 改图像布局，`startup` 打印旗标）。**不要用 `force_blackhole`**：它只是把稀疏缓冲/图像映射到一页「黑洞」内存，**并不跳过片段着色**（2026-10-30 的 `tr3` 白跑一轮，读了 Mesa 源码才确认） |
| `mali-skip-format=N` / `mali-skip-pipeline=P/P…` | 跳过所有绑了 N 号像素格式纹理的绘制（500 = 壁纸的两平面 YCbCr）/ 跳过管线标签（最后一个点之后的部分，如 `PBGRAXm_A2Xhf`）匹配的所有绘制（模式里的 `.` 代表任意文本，`.Tds4.` = 标签里含 Tds4 的）；`mali-list-pipelines=1` 把每个不同的标签打印一次。用来量「去掉这一类绘制，GPU 时间还剩多少」 |
| `mali-trace=print` | Mesa 的 u_trace（`MESA_GPU_TRACES`）：每个命令缓冲、渲染通道（尺寸、格式、tile 大小）、屏障、cache flush、同步等待都带 GPU 自己的时间戳；插件里的 `traceWrite`（funopen）在内存里汇总成 `gt …` 行（`gt R WxH rtN FORMAT tileN DURus` 是通道，另有 `gt dispatch`、`gt meta`、`gt cb`…），再用脚本按形状汇总 |

**`mali-trace` 只能当诊断用**：u_trace 的克隆堆（`PANVK_UTRACE_CLONE_MEM_SIZE`，插件设成 32 MiB）在 backboardd 活了约 100–115 s 后用光，Mesa 在 NULL 上解引用，backboardd 段错误。
**而 backboardd 一死，内核的显示后端 5 秒后报 `IOMFB: swap_wait_gated: CommandWake didn't happen … cancelling swaps`，接着 `native_legacy_backend_guard` panic**（`leaf=345`，`LegacyDisplayProbe.cpp:508`；perf5、perf8 都是这么死的）。
也就是说：**任何让 backboardd 崩的事现在都会把整个系统 panic 掉**——这是显示后端的健壮性缺口（取消交换那条叶子没实现），没修。跑 trace 要让手势排在 backboardd 起来后 ~100 s 内（`evb10-touch-demo=140`，手势从内核 162 s 起）。
backboardd 的沙箱会杀掉在 `pipe()` 和「给 fd 2 建文件 / dup2」上的进程（reason 25），所以重定向输出一律用 `funopen()` 赋给 `stdout`/`stderr`。

**原因（`perf6` 的 `gt` 行证实）**：重帧里每个采样壁纸的编码器都有一个 **73–77 ms 的 `gt dispatch`**（514 个里 ~100 个）。壁纸是 `J120.HEIC`，
3416×3416、Metal 像素格式 500（`YCBCR8_420_2P`）。metal2vulkan 把它变成 `R16G16B16A16_SFLOAT` 影子图像（93 MB），影子的「拷入」是一次转换：两个平面拷进暂存缓冲，再一个计算 dispatch 写满每个像素。
它的文档（`ycbcr.rs` 的 *When it converts*）写得明白：**CPU 可以不声不响地改 IOSurface 的字节，运行时没法便宜地知道没变，所以每个用到这个纹理的编码器都转换一次**。
一帧里有几个编码器采样壁纸就转几次，壁纸那一帧的 GPU 时间约 90% 在转换上（`rp1 dr2` 的 96 ms 对 3 ms 的同形状命令缓冲）。

**修**（`m2v-patches/0100-ycbcr-skip-unchanged-conversion.patch`，`apply.sh` 按文件名顺序打 `0*.patch`；插件开机参数 `mali-ycbcr-memo=N`，默认 4096，0 关掉）：
新文件 `ycbcr_memo.rs`。转换记下上次的 (平面字节的采样哈希, 该缓冲被「写」的次数)，两者都没变就不录转换（use 照报，tracker 的顺序不变）。

- **哈希**：从缓冲的主机地址（`Buffer::contents()`）读平面字节里均匀分布的 N 个 64 字节块加最后一块；块数 ≤ N 时全哈希。能抓到「整帧都变」（解码器的帧）；**抓不到两个采样块之间的小区域改动**（想要精确就把 N 开大）。
- **GPU 写**：`EncoderUses::record`（所有编码器、所有命令记 use 的唯一入口）里，凡是 `write` 非空的 use，对这个缓冲的 key（以及它的 alias key，主机内存合并的那个）计数加一，不管 tracked 与否、不管区间。
  计数在编码时就加，所以同一编码器里转换之后又写平面、或之前的命令缓冲写过平面，下一次转换都看到计数变了而重新转换。
- **没看到的**：录了但没提交的命令缓冲（编码失败、`reencode.rs` 的重编码）会让备忘以为图像里有转换而其实没有；再换字节或有写就会恢复。两个线程同时编码、提交顺序和编码顺序不一致时，后提交的可能先跑。
- 只有 `contents()` 有主机地址（非 `Private`）的缓冲才备忘。

**结果**（同样的负载 `evb10-touch-demo=140 evb10-vop-direct=1 mali-div=1 evb10-vop-fence=1`，没开 trace；`perf1` 修前、`perf7` 修后）：

| | `perf1`（修前） | `perf7`（修后） |
| --- | --- | --- |
| 手势期间每 5 s 交给显示的帧数 | 4–15（平均间隔 ~115 ms） | **18–31**（平均间隔 26–56 ms，最忙一秒 24 帧） |
| K 行里 GPU ≥ 60 ms 的 | 8 个（最大 96 ms） | **0 个**（最大 54 ms） |
| `gt dispatch` ≥ 5 ms（`perf8`，开 trace） | 约 100 个 73–77 ms（`perf6`） | **1 个** |

**还剩（当时的判断，已被推翻）**：`perf8` 里 1072×1904 的 rt2 通道有 **96 个固定 28–30 ms**（同形状的普通通道 1–3 ms），一帧约 40–50 ms 的 GPU 里有一个。这是那张 93 MB 的 F16 壁纸被缩小 3.2 倍采样的全屏通道。
当时以为「时钟不敏感，所以多半是内存/MMU 的事」，试了 2 MiB 块映射：没有改善（Mali 仓库的笔记里有）。**真正的原因是 m2v 在片元着色器里逐位仿 Metal 的 YCbCr 线性滤波（约 570 条指令一次采样），是算力，不是内存**——见下一节。

## 帧率：滑动稳态 38 → 50 fps，最好的手势 58 fps（2026-10-30，`板子运行 clk750、ycf*、ycx*、shd*、vb4/vb5、fp1–fp3、ord1/ord2、cp1–cp3、fv1/fv2`）

标记：**[板上]** = 在板子上观察到的；**[主机]** = 只在主机上验证的（构建、补丁打得上、脚本）。

**指标**：`evb10-touch-demo` 的手势（先上滑解锁，然后左右交替各滑 0.8 s，每 7 s 一次）。**手指按下的 0.8 s 里面板实际显示的帧数**（`frame_phases.py TAG`），取第 3–11 个手势
（第 0–2 个手势是解锁和启动期，帧数波动很大，不计）。同时看「相邻帧间隔 > 25 ms 的个数」和「命令缓冲 GPU 时间的中位数」。命令缓冲的时间线要开 `mali-timeline=1`。

| 运行 | 与上一行的区别 | 平均 fps | 间隔 > 25 ms | 中位 CB 的 GPU 时间 |
| --- | --- | --- | --- | --- |
| `tl1` | 基线：GPU 750 MHz、`mali-ycbcr-filter=device`、PanVK 旧的 FPK 规则 | 37.9 | 89 | 14.1 ms |
| `fp1` | 同 `tl1`（重复一次，看运行间波动） | 40.0 | 80 | 14.1 ms |
| `fp3` | FPK 原型（`mali-panvk-fpk=always`：只挡目标读；已不在补丁里） | 44.7 | 51 | 9.6 ms |
| `cp1` | FPK 终版（Mesa 0011，默认） | 43.2 | 63 | 9.7 ms |
| `cp2` | A76 优先的核编号（见 3.2） | 47.5 | 37 | 9.7 ms |
| `cp3` | 再加 `cpumask=0x1f`（只开 5 个核：4 个 A76 + 引导核） | 50.3 | 19 | 9.7 ms |

同配置的两次（`tl1`、`fp1`）差 2.1 fps：**单个平均值的误差约 ±2 fps**，`cp1→cp3` 里「间隔 > 25 ms 的个数」（63 → 37 → 19）比平均值更可信。最好的一个手势：`cp2` 58.8、`cp3` 57.5 fps。
起点（`perf1`，修 YCbCr 转换之前）是约 100 ms 一帧、8–10 fps；`perf7` 之后平均交给显示的间隔 26–56 ms。
**上面全是用 `EXTRA_KERNEL_ARGS` 加的参数量的**（`evb10-vop-direct=1 evb10-vop-fence=1 mali-pll=1 mali-div=1 mali-ycbcr-filter=device`）；`board.toml` 的默认参数没有改，
**不带这些参数时跑的仍是拷贝式扫描、GPU 198 MHz**。复现：

（这是这一节当时的写法；现在帧率参数都在 `board.toml` 的默认里，新的写法见「帧率（二）」第 9 节。）

```
cd uefi-evb10 && ARGS="evb10-touch-demo=120 evb10-vop-direct=1 mali-pll=1 mali-div=1 evb10-vop-fence=1 evb10-bt=x evb10-bt2=x evb10-bt3=x evb10-bt4=x mali-ycbcr-filter=device mali-timeline=1"
(FV=$PWD/out/fv.bin CAPTURE_SECS=235 EXTRA_KERNEL_ARGS="$ARGS" setsid nohup tools/xnu-probe/run-kernel.sh TAG > runs/TAG.driver.log 2>&1 </dev/null &)
/usr/bin/python3 tools/xnu-probe/frame_phases.py TAG      # 每个手势一行，末尾两行是汇总
```

### 1. GPU 这一边

**1.1 时钟（`mali-pll=1 mali-div=1`，Mali 仓库 `aa7028f`）[板上]**：`mali-pll=N` 选 `clk_gpu_src` 的源（0 GPLL 1188 MHz，1 CPLL，2 AUPLL，3 NPLL），`mali-div` 是分频（默认 5 → 198 MHz；1 → GPLL/2 = 594 MHz；
CPLL/2 = 750 MHz）。电压是 UEFI 设的 0.75 V，没有动；厂商的工作点表里 800 MHz 对最差的片子也是 0.75 V，所以 750 MHz 不需要升压——但**这不是调频**，只是把固定频率抬高。
同一个 1072×1904 的重通道：198 MHz 84.9 ms，594 MHz 28.4 ms，750 MHz 22.5 ms（周期计数器探针读到 750 MHz）。**重通道的时间严格跟时钟走**；`perf` 一节里「198 与 594 MHz 帧时间一样」的说法是在转换 dispatch 占着一帧时量的，不能外推。

**1.2 壁纸通道是 YCbCr 滤波的算力（m2v 补丁 0101，`mali-ycbcr-filter=device`）[板上]**：Metal 对两平面 YCbCr 纹理的线性采样，m2v 在片元着色器里**逐位仿**（`bind/plan/slots.rs`），`mali-bifrost-debug=shaderdb`（`gpu_shaderdb.py`）数出来
**约 570 条指令一次采样**。壁纸（`J120.HEIC`，3416×3416）的全屏通道在 750 MHz 要 22.5 ms；`device` = 用设备自己的线性滤波器去采转换后的影子图，**7.9 ms**。代价：不再和 Metal 逐位一致。**静态锁屏截图比较**（`fx1` = exact、`fv1` = device，其余参数相同；RGB565，4:1 平均）：127568 个点里 1483 个不同（1.2 %，壁纸区域 1159 个），
最大差 9/255（RGB565 的一级是 8），平均差 0.07——和截图自己的量化噪声同量级；同一张主屏小组件页（第 9 张）也是这样。默认不变（`exact`）。
被反驳的假设：纹理带宽（把影子图存成 8 位的实验没有改变帧时间，补丁已删）、页表遍历（2 MiB 块映射）。

**1.3 通道和填充的底噪（`vk_bench.c`，`usertest`）[板上]**：750 MHz 下 1072×1904 BGRA8——一个没有绘制的通道 0.136 ms；常量色全屏填充每次 0.026 ms（79 Gpx/s），带混合的每次 0.237 ms（8.6 Gpx/s），
很重的 ALU 着色器每次 0.133 ms（15 Gpx/s）。**混合/重着色器的全屏层才是贵的**；常量色的层几乎白送。这也是 FPK 值钱的原因：被后面的不透明全屏层盖住的层，不杀就得白画。

**1.4 前向像素杀（FPK）：旧规则几乎让 CA 的 pass 用不上它（Mesa 0011）[板上]**：CA 的 pass 都绑着一个无内存的第二颜色附件（写掩码 0 / 不写）。PanVK 旧规则：`allow_forward_pixel_to_kill` 要求**所有绑定的颜色附件都被写**，
于是这些 pass 里的绘制全都不允许杀别人，叠着画的全屏层全部照跑。G610 的实际判据（`vk_bench.c` 的杀伤矩阵，两个目标，受害绘制是重着色器，杀手是 4 次常量填充；「杀」= 通道变便宜）：**后画的写集合 ⊇ 先画的写集合才杀**，部分写掩码按「读目标」算：

| 受害者的写集合 \ 杀手的写集合 | {0} | {1} | {0,1} |
| --- | --- | --- | --- |
| {0} | 杀 | 不杀 | 杀 |
| {1} | 不杀 | 杀 | 杀 |
| {0,1} | 不杀 | 不杀 | 杀 |

（旧规则只有最后一列杀。）Mesa 0011：去掉「所有绑定附件都被写」这一条，保留「着色器能 FPK、没有 alpha-to-coverage、没有目标读/混合、没有读后写」几条；`PANVK_FPK=bound`（`mali-panvk-fpk=bound`）恢复旧规则做 A/B。
**正确性证据**：杀伤矩阵 9 对里，**只有受害者写的那些目标上，2041088 个像素 0 个不同**（`ord2`；不杀的那些对子也一样）；`fv1`（新规则）与 `fv2`（`bound`）的**静态**锁屏截图逐像素相同（`evb10-shot-img` 第 6 张，4:1 采样 127568 个像素 0 个不同；
第 5 张只有顶部时钟/日期带里差 ≤ 9）。同配置的两次不同运行里，静态锁屏截图本身也逐点相同（`shot1` 第 3 张 = `fv1` 第 6 张，0 个不同），所以这个比较的噪声底是 0。**动画中的帧无法逐像素比较**：截图是在串口上一行一行读活的帧缓冲，要几秒，运动的内容会撕开（白块边缘呈锯齿、时钟数字呈横纹，新旧规则的截图里都有），这不是渲染错误。
效果：命令缓冲 GPU 时间中位数 14.1 → 9.7 ms；平均 +4～6 fps（`fp3`、`cp1` 对 `tl1`、`fp1` 的平均 39）。**这是改变默认渲染行为的补丁，是否作为默认由人决定**（工作区里已是默认）。

### 2. 帧节奏（从时间线推出来的模型，不是直接量到的）[板上]

- 渲染服务器在上一个 vsync 之后约 **14 ms** 提交命令缓冲（编码 + 提交）；帧在 GPU 做完之后的**第一个 vsync** 显示。要赶上 V+2，提交到完成要 **≲ 19.3 ms**（两个 vsync 去掉那 14 ms 的偏移后剩下的）。
- swap 在命令缓冲「已调度」时交给显示，**在途深度约 2**：一次超时哪怕只有不到 1 ms，也会让相邻几帧一起晚一个 vsync，平均帧率掉约三分之一。
- 重的命令缓冲（16–17 个通道、93–95 个绘制，GPU 11–13 ms；在 A76 上重放 5.5–6.7 ms + 提交 1.8–2.9 ms）正好压在这条线上；轻的（10 个通道、64–81 个绘制）约 15 ms，稳稳赶上。每个手势的头几帧是重的。

### 3. CPU 这一边

**3.1 提交到 GPU 开始之前的时间都在渲染服务器的编码线程里 [板上]**：时间线字段 `rep.sub.sch.ca.cb`（`mali-timeline=1`）：`rep` = 提交（commit）之后编码线程多久才开始处理它并把编码器的调用**重放**一遍，`sub` = 运行时录制 + 提交花多久，
`ca`/`cb` = 开始/结束时线程所在的核。提交结束到 GPU 第一个时间戳只有约 **0.7 ms**（p99 1.3 ms，最大 1.6 ms，GPU 空闲时 662 个里没有一个 > 2 ms），**没有门铃丢失之类的延迟**。
所以 `commit → GPU 开始` 的 6 ms 基本是 `rep + sub`。

**3.2 它落在了 A55 上 [板上]**：XNU 按 ADT `/cpus` 子节点的顺序给核编号，调度器偏爱编号最小的空闲核；RK3588 的 A55（物理 0x000–0x300）排在前面，所有重放都在 A55 上（`cp1`：100 %）。`cpu_bench.c`（`usertest`，核号读 `TPIDR_EL0[11:0]`，前后各读一次，中途换核的不算）：

| | ALU（ns/次） | L2 内指针追逐 | 分支混合（128 KiB） | 复制 GB/s | 16 MiB 追逐 | 16 MiB 混合 |
| --- | --- | --- | --- | --- | --- | --- |
| A76（逻辑核 0–3，~1775 MHz） | 2.38 | 4.7 | 11.1–11.5 | 3.6–4.3 | 103–108 | **30.6** |
| A55（逻辑核 4–7，~1861 MHz） | 2.69 | 4.5 | 15.5 | 2.6–2.7 | 94–96 | **96.0** |

**不规则、访存多的代码 A76 快 3.1 倍**（编码线程恰好是这种）。`cp1`：重放中位 6.8 ms、提交 3.0 ms；`cp2`（A76 优先）：重放 4.2 ms（A55 上 11.4、A76 上 3.8）、提交 1.4 ms；
提交到完成（`ctc`）中位 19.2 → 15.5 ms，p90 29.1 → 24.0 ms（只统计 GPU > 2 ms 的命令缓冲）。开机也快了：`bundle loaded in backboardd` 从 92–95 s 到 71–76 s。

修法（三处，**缺一不可**，`UniXNU-rk3588-common`）：
1. `dts/rk3588.dtsi`：`cpus` 里 A76（cpu4–7，0x400–0x700）排在前面，A55 在后面。**这一条改变默认的核编号**（工作区里已是）。
2. `patches/xnu/rk3588-boot-cpu-mpidr.patch`：UniXNU 的主机工具（`scripts/unixnu/devicetree.py`）把**第一个**启用的 cpu 节点标成 `state = running`，XNU 的 `ml_is_boot_cpu()` 就拿它当引导核；
   固件实际在 A55 0x000 上启动，两者不一致（逻辑 4 → PSCI -4 `ALREADY_ON`，只起来 7 个核）。补丁让 XNU 按 `MPIDR_EL1` 的亲和性找 `reg` 相同的节点（节点列了引导核时），否则回到原来的判据。
3. `scripts/data/ArmCpu-rk3588-boot-cpu-slot.patch`（构建时对 `drivers/ArmCpu` 子模块打）：引导核不是逻辑 0 时，槽位 0 也是要起的从核（`drainPending` 的循环从 0 开始）。

**3.3 `cpumask=0x1f`（标准 XNU 启动参数）[板上]**：只启用逻辑 0–4（4 个 A76 + 引导的那个 A55）。`cp3`：50.3 fps、间隔 > 25 ms 的 19 个；重放落在 A55 上的比例 23 % → 8 %，提交到完成 p90 24.0 → 20.8 ms。
代价是关掉 3 个 A55（吞吐少了）；**只当 A/B 用，不是默认**。更好的办法是让高 QoS 线程避开 A55 又不关核（调度策略，没做）。

### 4. 还剩什么（都没做）

- **流式提交**：现在整个命令缓冲编码完、重放完才提交，GPU 在 `rep+sub`（约 6 ms）里空等。让 GPU 在重放的同时就开始跑前面的通道，能把重帧的 `commit → 完成` 压回 19.3 ms 以内。m2v 的 `command_buffer/submit.rs` 已有「Metal 的提交批次」的概念，要读懂再动。
- **重放本身**：A76 上每个绘制约 40–75 µs，偏高；还没有采样器（用户态 `thread_get_state` 采样编码线程的 PC，再用 `nm` 对插件符号，是最便宜的办法）。
- 去掉那个死的 memoryless 附件 c1（CA 每个 pass 都带着，没人读）；GPU/CPU 调频（需要 PMIC/电压支持）；GPU 完成中断（现在轮询）。
- `mali: STALL group …`（有作业 ≥ 2 s 没完成）在 48 份旧日志里有 19 份出现，包括 FPK 之前的；环的 extract 落后 insert 一个作业（`cs req 0x511 ack 0x11`：空闲位没应答，像是固件以为队列已空）。时间线里每个命令缓冲提交后 0.7 ms 内都开始了，所以它多半出在空闲后的第一个作业上；原因未查。

### 5. 新工具、启动参数和坑

- 分析脚本（`uefi-evb10/tools/xnu-probe/`）：`frame_timeline.py TAG`（命令缓冲和面板帧放到同一条时间轴）、`frame_phases.py TAG…`（每个手势的 fps / 间隔 / GPU 忙 / 中位 CB）、`gpu_passes.py`（`mali-trace=print` 的通道）、`gpu_cbcmp.py`（按通道数分组的 GPU 时间对比）、`gpu_shaderdb.py`（`mali-bifrost-debug=shaderdb`）。
- 启动参数：`mali-timeline=1`、`mali-panvk-fpk=bound`、`mali-ycbcr-filter=device`、`mali-bifrost-debug=shaderdb|statsfull`、`mali-cache=NAME`（换一个空的着色器缓存目录，强制重新编译）、`mali-skip-format=N`、`mali-skip-pipeline=P/P`、`mali-list-pipelines=1`、`mali-pll`/`mali-div`、`cpumask=0x1f`。
  `usertest` 的 `gpu-test` 末尾现在会跑 `vk_bench` 和 `cpu_bench`（只报告，不影响通过与否）。
- **同一时刻只能有一个 `run-kernel.sh`**：它们共用板子宿主机上的串口和 `/tmp/probe-payload-boot.img`；启动前 `pgrep -af run-kernel` 必须为空，也别在 Shell 里用 `sleep` 循环等它（会被转后台，然后下一条命令就误启动第二个）。
- 改了 XNU 补丁列表会重建 `.cache/xnu-work`，`prepare-xnu-source.py` 要从 GitHub 重新下 DriverKit 头文件；`env -i` 会丢掉代理环境，构建内核要把 `https_proxy`/`http_proxy` 传进去（`tmp/chain-k.sh`）。
- `evb10-shot-img` 截图不是瞬间快照（见 1.4）：动画中的帧会撕开，比较要用静态帧。



## 帧率（二）：编码线程别落到 A55 上，GPU 1 GHz（2026-10-30，`板子运行 gp1 ps0 sl2 sl3 ep1 ep2 ep3 gp2 tk4 soak1`）

标记同上：**[板上]** = 板子上观察到的；**[主机]** = 只在主机上验证的。上一节结尾「还剩什么」之前，`cpumask=0x1f` 之后重放还有约 8 % 落在 A55 上，这是尾巴里最大的一块；这一节把它量清楚、修掉，
顺手把 GPU 抬到 1 GHz，并记下没有用的东西。**所有数字都是 `evb10-touch-demo=120` 的手势，板子上跑出来的，每个配置只跑了一两次（运行间的波动见下）。**

### 0. 结果 [板上]

**指标换了。** 「手指按下的 0.8 s 里显示的帧数」被触摸到第一帧的延迟卡住（见第 7 节：右滑要 180–200 ms 才出第一帧），节奏再好，右滑也只能读到 48–51 fps，左滑 55–60 fps。
所以这一节改看三样：**重的命令缓冲（≥ 50 个绘制）从提交到完成的 p50 / p90**、**超过 19.3 ms 的比例**（第 2 节：再长就赶不上 V+2，丢一帧）、**重放落在 A55 上的个数**（时间线 `ca` ≥ 4，`cb_table.py`）。
手势 3–11（9 个）里相邻帧间隔 > 25 ms 的个数也列出来，但它很少（0–15 个），噪声大。

| 运行 | 配置（所有运行共有：`evb10-vop-direct=1 evb10-vop-fence=1 mali-ycbcr-filter=device cpumask=0x1f mali-aflush=1 mali-timeline=1`，GPU 850 MHz） | fps | 间隔 > 25 ms | 提交→完成 p50 / p90 | > 19.3 ms | 重放在 A55 上 |
| --- | --- | --- | --- | --- | --- | --- |
| `gp1` | `evb10-ps=10`（当时 `board.toml` 默认带着的调试采样器） | 53.3 | 3 | 15.7 / 21.0 ms | 13.0 % | 46 / 469（10 %） |
| `ps0` | `evb10-ps=0` | 52.5 | 9 | 15.2 / 18.5 ms | 6.6 % | 19 / 470（4 %） |
| `sl2` | ps0 + `sched-slowcpus=0xf0` | 52.5 | 15 | 16.2 / 20.4 ms | 13.6 % | 70 / 434（16 %） |
| `sl3` | 同 `sl2`（多了调度器的诊断输出） | 53.1 | 12 | 16.1 / 19.7 ms | 12.3 % | 61 / 471（13 %） |
| `ep1` | ps0 + `mali-epri=50` | 51.8 | 13 | 16.0 / 25.6 ms | 19.2 % | 86 / 469（18 %） |
| **`ep2`** | ps0 + `mali-epri=50 sched-slowcpus=0xf0` | 53.5 | 3 | **13.7 / 17.3 ms** | **3.5 %** | **2 / 425（0.5 %）** |
| `ep3` | `ep2` + 栅栏等待每 100 µs 看一次（代码已撤，见第 6 节） | 53.3 | 5 | 14.0 / 17.6 ms | 3.6 % | 9 / 500（1.8 %） |
| **`gp2`** | `ep2` + `mali-npll=1000`（GPU 1000 MHz） | 54.0 | 4 | **12.7 / 15.6 ms** | **1.6 %** | 6 / 488（1.2 %） |
| **`v1`** | **提交后的 `board.toml` 默认**（上面这些参数都在默认里，GPU 750 MHz，没有 `evb10-ps`），只加了 `evb10-touch-demo=120 mali-timeline=1` | 52.6 | 11 | 14.9 / 18.9 ms | 8.7 % | 6 / 504（1.2 %） |
| **`v2`** | `v1` + `mali-pll=3 mali-div=0 mali-npll=1000`，固件按 `docs/reproduce.md` 的步骤重编（`GPU_RAIL_UV=850000`，0.85 V） | 53.8 | 4 | 12.7 / 15.6 ms | 2.5 % | 9 / 511（1.8 %） |

`ep2` 和 `ep3` 是同一个配置跑了两次，三项指标基本一致（3.5 / 3.6 %，A55 上 2 / 9 个），这个差别（对 ps0 的 6.6 % 和其它的 12–19 %）不是运行间噪声。
手势里面板上的帧间隔基本是一个 vsync：`ep2` 的第 3–11 手势平均间隔 16.6–17.8 ms，`gp2` 的最好一个手势 60.0 fps（48 帧 / 0.8 s，0 个长间隔）。

**GPU 频率的取舍**（`ep3` / `gp2` / `v1` 的配置只差 GPU 频率，A76 都在 2.23 GHz）[板上]：

| GPU | 重命令缓冲的 GPU 时间（中位，`cb_table.py` 的 `gpu50`） | 提交→完成 p50 / p90 | > 19.3 ms | 手势 3–11 的 fps |
| --- | --- | --- | --- | --- |
| 750 MHz（默认，0.75 V） | 10.1 ms | 14.9 / 18.9 ms | 8.7 % | 52.6 |
| 850 MHz（`mali-pll=3 mali-div=0`，NPLL 的固件值；`ep3`，在 0.85 V 的固件上测的，0.75 V 没有测） | 9.1 ms | 14.0 / 17.6 ms | 3.6 % | 53.3 |
| 1000 MHz（0.85 V，`gp2`；`v2` 是按文档步骤重编的固件，同样 12.7 / 15.6 ms，2.5 %，53.8 fps；`soak1` 12 分钟 1.4–1.9 %） | 7.9 ms | 12.7 / 15.6 ms | 1.6 % | 54.0 |

平均帧率三档差不多（手指按下期间的节奏被触摸到第一帧的延迟和显示的 vsync 卡住），差别在**赶不上期限的帧**：750 MHz 下每 11–12 个重帧丢一个，1 GHz 下约每 40–60 个丢一个。每个配置只有一次 4 分钟的运行（1 GHz 另有 12 分钟的）；`ep3` 和 `gp2` 在 0.85 V 的固件上跑，`v1` 在默认的 0.75 V 固件上跑。

### 1. 为什么重放会落在 A55 上，怎么修 [板上]

**两个事实**：(1) XNU 的 `choose_processor` 有空闲核就用空闲核，不去抢占别的线程。A55（逻辑核 4，`cpumask=0x1f` 之后只剩这一个）几乎总是空闲，所以快核全忙时编码线程 E 就被送到那里；
(2) 线程倾向回到上次运行的核：`ep1` 里 E 一旦落到 A55，会连着留下（最长连续 20 个命令缓冲，`ps0` 最长 4 个，多数是单个）。A55 上重放 14 ms（A76 上 4 ms），这一帧就丢了：`ep1` 的 56 个 > 21 ms 的重命令缓冲里 51 个在 A55 上。

**两半缺一不可**（上表的 2×2）：

| | 不回避慢核 | `sched-slowcpus=0xf0` |
| --- | --- | --- |
| E 优先级不动（UI QoS，46） | `ps0`：4 % 在 A55，6.6 % > 19.3 ms | `sl2/sl3`：13–16 %，12–14 % |
| E 优先级 50（`mali-epri=50`） | `ep1`：18 %，19.2 % | **`ep2/ep3`：0.5–1.8 %，3.5–3.6 %** |

- **`sched-slowcpus=MASK`**（XNU 补丁 `rk3588-slow-cores.patch`，`UniXNU-rk3588-common`）：给调度器一个「慢核」的逻辑核掩码（A76 优先的编号下 A55 = 4–7 = `0xf0`）。基础优先级 ≥ `sched-slowpri`（默认 37：UI 和 UIN 两个 QoS 类）的线程，
  有空闲的快核时不去空闲的慢核；没有空闲快核但有快核在跑更不重要的线程（可以抢占的）时，抢占那个快核；也不因为「上次在慢核上跑」回到慢核。所有快核都在跑同级或更高的线程时照旧落到慢核（等快核有时可能比慢核更久）。
  默认 0 = 不起作用。`sched-slowstat=N` 让维护线程每 N 秒打一行计数。**只开它没有用**：E 是 46，快核上跑的 SpringBoard 线程是 47（前台应用加一），没有可以抢占的，仍然落到 A55，而其它 UI 线程现在也不去 A55 了，反而更挤。
- **`mali-epri=N`**（插件，`NativeBridgeMetalFrontend.mm` 的 `encodeThreadPriority()`）：用 `THREAD_PRECEDENCE_POLICY` 把 E 的优先级设成 N（任务基础优先级 31 + importance）。控制台会打一行
  `encode thread priority: the task's 31, wanted 50 (importance 19), now 50, result 0`。50 高于 SpringBoard / locationd 的 47，低于显示线程 D（54–55）和 backboardd 里被提升的线程（63）。**这样 E 会失去 QoS**（回到默认桶），**只开它也没有用**：
  有空闲的 A55 时调度器不抢占，E 就去 A55，而且更黏（`ep1`）。
- 合在一起：E 有 47 的线程可抢，又没有路通到 A55（`ep2`：A55 上 2 个）。

**慢核回避的计数（`ep2`，235 s，`sched-slowstat`）[板上]**：基础优先级 ≥ 46 的线程被放置了 205 158 次（约 870 次/s）：27 % 有空闲快核，**71 % 要抢占一个快核**，2.3 %（4 774 次）没有快核可用、落到了 A55。
最后这些里 SpringBoard 的主线程（47）最多（取样的 220 个事件里 120 个），其次是 locationd 的 CoreMotion 线程；**代价**是这几次 SpringBoard 主线程在慢核上跑，帧节奏上没看到影响（间隔 16.6–17 ms）。

### 2. 快核被谁占着 [板上]

`sl3` 的诊断输出（每次「UI 类线程找不到快核」时记下 8 个核上跑的线程）：SpringBoard 主线程（47）和它的 `uikit.eventfetch`（47）、backboardd 里被提升到 63 的线程、显示线程 D（54–55）、E（50）、
locationd 的 `CoreMotion`（47）。**内核线程也在里面**：`thread call high #n`（93）和 `thread call kernel-`（92）占了这些时刻快核槽位的 7–11 %（`sl3`、`ep2`）。

其中一个已经量到：触摸驱动（`Evb10Touch.cpp`）每约 11.5 ms 一次 `thread_call`，轮询 I2C5（主控是轮询的，`IODelay(1)` 忙等），**100 kHz 下每次 535 µs**（每 1000 次里最长的一次：`ep3` 1.2 ms、`gp2` 6.4 ms、`soak1` 4.8 ms，包含被抢占的时间），约占一个快核的 4.6 %，优先级 93。
`evb10-touch-khz=400`（新启动参数，默认 100）：每次 **150 µs**（最长 191 µs），`tk4` 里 I2C 扫描照旧看到 0x14 和 0x5d，Goodix GT1158 的产品号和配置读出来了（`product id 1158`，10 个触点），0 个总线错误。
**手指真的按上去没有测**（要人动手）；总线是同一条，多读的只是更多字节。

### 3. 调试采样器的代价 [板上]

原来 `board.toml` 默认有的 `evb10-ps=10`（每 10 s 打进程表，加上每个「忙」的进程 12 × 8 行线程栈；现在不在默认里，要用时 `EXTRA_KERNEL_ARGS=evb10-ps=10`）自己是 `kernel_thread_start` 的 95 优先级线程，一次运行多打 3.3 MB（关掉是 0.3 MB），串口 1.5 Mbaud 上忙等。
`gp1` 对 `ps0`：重放在 A55 上 46 对 19 个，> 19.3 ms 13.0 % 对 6.6 %。**量帧率时别开它**（现在默认就是关的）。 关掉后 `kernel_task` 剩下的主要就是上面的触摸 tick。

### 4. GPU 1 GHz：`mali-npll=1000`（Mali 仓库）[板上]

NPLL 固件设在 850 MHz（M = 425，P = 3，S = 2，VCO 3.4 GHz）。`mali-pll=3 mali-div=0` 是直接用它。`mali-npll=MHz`（400–1100）在切到 NPLL 之前只改乘数 M（`M = MHz × P × 2^S / 24`，1000 MHz = M 500，VCO 4 GHz，
在 PLL 的 2.25–4.5 GHz 范围内），等锁定位（`CON6[15]`）；没锁住就恢复原来的 M。GPU 此时还没有接到 NPLL，别的东西也不用它。控制台：
`mali: mali-npll=1000: m 425 -> 500, npll now 1000000 kHz (locked 1, con 0x1f4 0x83)`，GPU 周期计数器探针读到 `1000 MHz`。**电压**：厂商的工作点表里 ≤ 800 MHz 要 0.75 V、900 MHz 要 0.80 V、1 GHz 要 0.85 V；没有调频，所以 `vdd_gpu_s0` 开机时一次设好。
UEFI 里它现在是构建选项 `GPU_RAIL_UV`（微伏，默认 750000 = 0.75 V，`RockchipPlatformLib.c`）：跑 1 GHz 要 `GPU_RAIL_UV=850000` 重编固件。**默认是 750 MHz / 0.75 V**（`board.toml` 的 `mali-pll=1 mali-div=1`），1 GHz 是选项，步骤写在 `UniXNU-evb10/docs/reproduce.md`。
效果（`gp2` 对 `ep2`）：命令缓冲 GPU 时间中位 9.1 → 7.9 ms，提交→完成 p50 13.7 → 12.7 ms，p90 17.3 → 15.6 ms，> 19.3 ms 3.5 → 1.6 %。重通道的时间照旧跟时钟走。
**稳定性（`soak1`，12 分钟，`gp2` 的参数）[板上]**：81 个演示手势、3 553 个重命令缓冲，没有 panic、没有 `STALL group`，每 10 s 一行的 `vkpipe` 状态到最后一行（717 s，`n=76`）都是 `failed=0`；按时间四等分（`cb_table.py soak1 --split 4`）：
提交→完成 p50 12.5–12.6 ms、p90 15.7–15.9 ms、> 19.3 ms 占 1.4–1.9 %，GPU 时间中位 7.9 ms，重放起在 A55 上的 9–15 / 888（共 49 个，1.4 %），**没有随时间变慢的趋势**。
手指按下 0.8 s 的帧率，81 个手势平均 53.3 fps（左滑 55–60，右滑 48–52）。触摸 tick（100 kHz）6 万次平均 553 µs、最长 1.26 ms。`wdt-kick=1800` 在 30 分钟后停止喂狗，比这更长的采集要改它。

### 5. UEFI：A76 的电压表是全零（`uefi-evb10`，`UniXNUEvb10.dsc`）[主机] + [板上]

`PcdRk860xRegulatorTags|{ $(SCMI_CLK_CPUB01), $(SCMI_CLK_CPUB23), $(SCMI_CLK_NPU) }`：`SCMI_CLK_NPU` 只是 C 宏（`ScmiDefinitions.h`），DSC 里没有这个宏，整个数组编译出来是 `{0, 0, 0}`，
RK3588Dxe 找不到大核的稳压器，A76 簇被设成 2.4 GHz 的时钟却没升电压。改成 `0x6`（NPU 的 SCMI 时钟号）后数组正确。
**板上（`cb1`，`evb10-exec=1` 跑 `cpu_bench`，对 `ord2`）**：ArmCpu 校准出的 A76 频率 1773–1779 → 2223–2232 MHz（A55 不变，1861 MHz）；`cpu_bench` 里 A76 的 ALU 2.375 → 1.895 ns（−20 %）、
128 KiB 内指针追逐 4.73 → 3.78 ns、分支混合 11.1 → 8.9 ns；16 MiB 的混合（受访存延迟限制）30.6 → 28.5 ns，只快 7 %。**重放时间几乎没变**（`rep` 中位 3.9–4.2 ms）：编码线程的代码不规则、访存多，
主要卡在访存延迟上，不是核频率。也没有到固件设的 2.4 GHz（量到 2.23 GHz，原因没查）。空闲系统里新线程 32 次有 31 次落在逻辑核 0（A76），睡醒的主线程 22 次在核 0、10 次在核 1。


### 6. 试了没有用、代码已撤的 [板上]

- **`mali-fwpoll=100`**（栅栏等待每 100 µs 看一次，原来每 1 ms）：`ep3` 的提交→完成 p50 14.0 ms，`ep2`（不带）13.7 ms，没有差别。撤了。
- **`mali-nf=1`**（不做 `FLUSH_CACHE2`，只为量 GPU 时间）：没有跑，撤了。
- **只给 E 提优先级、或只开慢核回避**：上面的 2×2，都没有用。

### 7. 触摸到第一帧：右滑比左滑慢约 110 ms，和 GPU / 调度都无关 [板上]

演示手势从屏幕左侧 15 %（右滑）或右侧 85 %（左滑）起。**手势开始到第一个命令缓冲提交**：左滑 58–62 ms，右滑 150–180 ms；**到第一个连续动画帧**：左滑 81–93 ms（`ps0` 的中位数 151 ms，被一个手势拉高），右滑 178–207 ms。
这在所有运行里一样（`gp1 ps0 ep2 ep3 gp2`，`soak1` 的 40 对手势：首个命令缓冲 157 对 57 ms，首个连续动画帧 180 对 85 ms，中位数），GPU 提到 1 GHz 也没变，所以是 SpringBoard / UIKit 一侧的延迟，不是渲染。
**推测（没有逐线程验证）**：演示的第一个左滑（`g1`）没有产生帧，之后右滑 / 左滑交替——第一页没有后一页，右滑是在第一页向右进 Today View，左滑是退回来。
`gp1` 的进程表（每 10 s 一张，累计 CPU 时间）：演示开始前空闲 13.7 s 里 `WidgetRenderer_Default` 用户态 CPU 只多 0.003 s，演示期间每 13 s（一次左滑加一次右滑）多 0.17 s，`SpringBoard` 多约 0.7 s；
所以右滑启动慢的部分很可能是 Today View 的小组件在每次进入时重新渲染（SpringBoard 主线程加 chronod / WidgetRenderer 的 CPU 活），是软件的活，不是 GPU 也不是调度的问题。验证办法：同一个右滑里看这几个进程的线程栈（`evb10-ps` 表的时机和手势对不上，要给内核加一个手势触发的采样）。
这也是「手指按下 0.8 s 的 fps」读数右滑 48–51、左滑 55–60 的原因：节奏（相邻帧间隔 16.6 ms）两个方向一样。

### 8. 还剩什么

- **右滑启动延迟**（上一节）：推测是 Today View 的小组件在每次进入时重新渲染，还没验证。要在手势开始后的 150 ms 里看 SpringBoard 主线程、chronod 和 WidgetRenderer 的线程栈（`evb10-bt=SpringBoard` 的用户栈；要给内核加手势触发的一次采样，现在的 `evb10-ps` 是定时的）。
- 剩下的长间隔（`gp2` 手势 3–11 里 4 个）：一个命令缓冲 17–25 ms 的（重放被抢占或提交长），和两三个命令缓冲积压的；手势 2–3 里还有首次使用的 GPU 尖峰（28.8、50 ms，像是壁纸转换第一次跑）。
- **流式提交**（上一节第 4 条）：GPU 现在的余量大了（p90 15.6 ms，线是 19.3 ms），收益比之前小；仍然是让 GPU 在重放的同时开始跑前面的通道，约省 `rep + sub` 的一半。
- **触摸 tick 搬到 A55**：`thread_call` 回调不能指定核，要改成自己的内核线程 + `thread_bind`；`evb10-touch-khz=400` 先把它的成本砍掉了 3.6 倍。
- SpringBoard 主线程偶尔被挤到 A55（上面第 1 节的 4 774 次）：可以让 D / backboardd 的 63 优先级线程别占着快核。

### 9. 复现

`unixnu` 拒绝超过 **1000 字节**的内核命令行（`the kernel command line is 1018 bytes; the limit is 1000`，`payload` 步骤就失败，板子没有动）。上面这些帧率参数现在都在 `board.toml` 的默认里（884 字节），
再加 `evb10-touch-demo=120 mali-timeline=1` 是 921 字节；多加参数要删别的（`cpumask=31`、`sched-slowcpus=240` 已经用十进制省字节）。`EXTRA_KERNEL_ARGS` 里的参数替换 `board.toml` 里同名的。

```
cd uefi-evb10
(CAPTURE_SECS=235 EARLY_STOP=0 EXTRA_KERNEL_ARGS="evb10-touch-demo=120 mali-timeline=1" setsid nohup tools/xnu-probe/run-kernel.sh TAG > runs/TAG.driver.log 2>&1 </dev/null &)
python3 tools/xnu-probe/frame_phases.py TAG                 # 每个手势一行
python3 tools/xnu-probe/cb_table.py TAG --latency           # 提交→完成、> 19.3 ms 的比例、A55 上的重放、触摸→第一帧
python3 tools/xnu-probe/frame_timeline.py TAG --gesture 4 --before 60 --after 400
```

1 GHz：固件用 `GPU_RAIL_UV=850000` 重编（再 `tools/stub/build.sh`），`EXTRA_KERNEL_ARGS` 里加 `mali-pll=3 mali-div=0 mali-npll=1000`，完整步骤和控制台上该看到的行见 `UniXNU-evb10/docs/reproduce.md`。

表里的 `提交→完成` 和 A55 个数用 `uefi-evb10/tools/xnu-probe/` 里的 `frame_timeline.py` 的 `parse()` 算
（`mali-timeline=1` 的 `tl` 行：`rep.sub.sch.ca.cb` 里 `ca` 是提交开始时所在的核）；`mali-profile=1` 是 D / E 两个线程的采样器，`prof_report.py` 汇总（编码线程里没有一个热点：panvk 的录制、m2v 的重放、`mach_msg2_trap` 各占一部分，都是平的）。
