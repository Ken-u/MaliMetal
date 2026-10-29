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
  逗号在插件里转成冒号，下划线转空格。
- 命令缓冲的编号每次运行不一样（锁屏帧的序号会漂），要用宽窗口（`mali-dump=1,40`）加 `mali-dump-draws=1,12`。
- 内核 printf 超过约 255 字符会丢尾巴，转储每行已经切到 180。
- 截图：`evb10-shot-img=1`（默认 4:1 RGB565）；`evb10-shot-scale=1|2|4`、`evb10-shot-y=Y0`、`evb10-shot-rows=N` 取一块 1:1 裁剪（PNG 只含这些行，绝对 y = Y0 + png y）；
  转 PNG 用 `uefi-evb10 的 tools/xnu-probe/shot2png.py <log> <输出目录>`。

## 滚动一帧要 100 ms：壁纸的 YCbCr 每个编码器都重新转换（2026-10-29，`板子运行 perf1…`perf8`）

**症状**：滚动（主屏翻页）时真实帧率 8–10 fps，`mali: user: status: K…` 的 `ctc`（提交到回调）100–380 ms；GPU 时钟 198 与 594 MHz 帧时间一样，所以不是着色器算力。

**新增的量法**（都在插件里，只对 backboardd 生效，除非另说）：

| 东西 | 用法 / 含义 |
| --- | --- |
| K 行 `gpu=…us st=…us` | 一个命令缓冲的 GPU 耗时（两个时间戳：桥给每个批次开头结尾写的）和提交后多久才开始。`gpu=0us st=-1us` = 在途命令缓冲太多，没分到时间戳槽 |
| `mali-panvk-debug=…` | 设 `PANVK_DEBUG`（`force_blackhole`：整个片段着色都不做，用来看一帧是不是片段瓶颈） |
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

**还剩**：`perf8` 里 1072×1904 的 rt2 通道有 **96 个固定 28–30 ms**（同形状的普通通道 1–3 ms），一帧约 40–50 ms 的 GPU 里有一个。这是那张 93 MB 的 F16 壁纸被缩小 3.2 倍采样的全屏通道：
时钟不敏感，所以多半是内存/MMU 的事（客户端缓冲一直是 4 KiB 页映射，93 MB = 23 K 页，采样跨度大时 GPU 的 TLB 整个吃不下）。下一节试了 2 MiB 块映射：**没有改善**。

