# M2-C2：呈现资源生命周期

2026-10-04。图形代码前使用 real-time-rendering-advisor，选择与官方 WSI 证据见 [决定](M2_C2_Presentation_Decision.md)。本增量处理 presentation resource release，不测屏幕 scanout 或输入延迟。

## 能力与接口

`presentation.hpp/.cpp` 封装纯能力协商、启动策略和 present 返回值的入队分类。实例仅启用可用且依赖完整的 KHR/EXT surface maintenance；Device 只有在匹配实例族、swapchain maintenance device extension 和 feature 全部成立时才启用它，优先 KHR，否则 EXT。feature 查询与启用使用同一个 Vulkan 别名结构。仍使用既有 GPU 选择，不为扩展偷偷更换设备。

`--present-sync auto|fence|legacy` 经 run.sh 转发。默认 auto 自动使用 fence，否则报告缺失原因并走 legacy；fence 要求能力成立，缺失时启动失败；legacy 关闭可选扩展以验证退路。`--present fifo|mailbox|immediate|auto` 仍决定呈现模式，与资源同步策略分别配置。

SwapChain 持有每个图像的 render-finished semaphore；扩展路径另持有每个图像的 present fence 和 pending 状态。Renderer acquire 后借用 semaphore，绘制提交等待 acquire、信号 render-finished，再委托 SwapChain present。Renderer pipeline/target 重建不再销毁这些呈现对象。帧槽的 submit fence、完成 ID、查询交付与场景/UI 退休保持独立。

## 回收合同

1. 每次复用 present fence 前，完成上次已入队请求，更新计数，随后 reset；禁止把 pending fence 重用于另一次请求。每图像 semaphore 的复用仍依赖 acquire 与提交中的 acquire wait。
2. 使用 raw vkQueuePresentKHR 保留准确返回值，避免 Vulkan-Hpp 在抛出 WSI 错误时跳过 pending 登记。success/suboptimal/out-of-date/surface-lost 等规范规定的已入队结果都登记并最终等待 fence。
3. host/device OOM 表示拒绝入队，不能等待刚 reset 却从未提交的 fence。device-lost 标记终止状态，跳过后续无限 fence 等待；未知错误保留不确定状态，不声称 release proven。
4. resize 和退出先等待 GPU 提交，再等待本代所有已登记呈现 fence，收集帧查询并退休资源，然后替换/销毁旧目标与呈现对象。析构提供 drain 兜底；正常路径由 Application 明确调用。幂等 drain 不重复计数。
5. legacy 仍使用 WaitIdle，并明确标记 `presentation_release_proven=false`。这是兼容退路，未关闭无 maintenance 设备上的呈现释放证明。fence 完成只证明规范规定的资源释放条件，不能据此声称图像已在屏幕显示。

Render Debug 显示 backend、累计入队/完成与 pending；legacy 显示原因和证明边界。schema 4 保留原字段，增加 `present_sync_backend/reason`、`present_fences_enabled`、`presentation_release_proven`、`present_queued_count`、`present_fence_completed_count`、`pending_present_fences`、`present_fence_wait_count`、`legacy_present_drain_count`。计数覆盖当前 swapchain generation 的全部请求，含暖机；不等于仅测量区间行数。benchmark 遇到 resize 会明确中断，避免跨 generation 报告混淆。

## 验证与边界

CPU 覆盖实例依赖、族匹配、feature、KHR 优先/EXT 兼容、require-fence 拒绝、legacy 关闭和入队结果分类。真实 llvmpipe/X11 WSI 覆盖一/两帧、KHR/EXT/legacy、13 次请求与查询、每图像复用、Renderer 重建保持 semaphore、pending 标记、最终 drain 与重复 drain。OOM 使用实际未提交 fence 和生产记账入口注入返回值，断言不等待；这是状态合同测试，没有人为耗尽显存或制造真实驱动 OOM。

三次真实 resize 在创建下一代之前尝试旧代 acquire/present，然后 drain、替换并继续绘制，验证资产/backing 稳定及旧代 pending 清零。当前 X11 返回正常结果，未实际制造 OUT_OF_DATE；其入队分类由 CPU 合同覆盖。禁止 acquire 已退休 swapchain，不能用创建替换后再 acquire 旧 handle 来伪造该测试。

原生 iconify/零尺寸等待、真实 WSI 错误/device-lost、validation 与新 RenderDoc、RTX 4060 Ti 帧预算/显存/画质仍开放，等待用户换机通知。软件吞吐不用于硬件性能结论。下一增量为 M3-A typed mip、sampler/各向异性、alpha coverage 与 tangent/法线空间验收，随后 GGX prefilter 和 BRDF LUT。

## 本轮结果

Release/Ninja 构建成功；最终 17/17 CTest 通过（10 CPU + 7 llvmpipe/X11 WSI），完整顺序在软件呈现环境连续两次通过。保留实际 KHR/EXT/legacy、一/两帧、HDR/图/UI/资产/查询/退休/resize/归零合同；原场景事务仍为 4 commits / 6 frames / 5 uploads / 6 image copies / 0 runtime upload waits。

报告烟测 one/auto、two/fence、two/legacy 分别 15/23/17 帧，全部行有独立有效查询。四合院原生 1920×1080/UI/orbit/FIFO/require-fence 一/两帧各 5 帧、3 个 swapchain images；KHR 入队与完成各 5、pending=0、release proven=true、legacy drain=0。最终账本与 M2-C1 同配置完全相同：53 images/views、17 backing blocks / 302,722,140 bytes；一帧 payload 295,481,120 bytes，两帧仅多 448 bytes、一 buffer/descriptor set。信号量/fence 属于驱动对象，未把它们虚算成 VMA image/buffer payload。

源码 SHA256 `5c2e25bb6121d9e0470c21c87c6d0ea65c362a925605c81bf6ccc8b9d1ff8673` 与构建头、两个保留的四合院报告一致。日志、验证 JSON 和任务增量 patch 在聊天工作区 outputs。本增量与 M2-B 至 M3-C1 纳入同一次集成提交；原始验证记录保留。

## 软件呈现条件及原生异常

验收命令明确设置 `VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json`、取消 WAYLAND_DISPLAY，并设置 `MESA_VK_WSI_DEBUG=sw,noshm`。这是可显示窗口的真实 X11/Vulkan WSI，未启用 headless swapchain；不修改普通 run.sh/App 的驱动或呈现路径。

首次两次完整顺序的 EXT case 超时 60 秒；独立 12 次重复以及仅 legacy/async 前缀未复现。GDB 保留完整六项前缀，抓到 Renderer::beginFrame 的 acquireNextImage 阻塞，而非 present fence 等待。测试补上与 Application 一致的逐帧 GLFW 事件处理后，暴露异步 `BadDrawable`（opcode 147 / minor 4 / pixmap 0x2000012）；初始 poll 和改 Xlib surface 均未解决。实际 XQueryExtension 核对 Present=146、DRI3=147，故错误是 DRI3 FenceFromFD，不能把它归为 Present QueryCapabilities，也未证明是帧槽/呈现 fence 重置错误。

从 [Mesa 26.2.3 官方源码包](https://archive.mesa3d.org/mesa-26.2.3.tar.xz) 核对 wsi_common.c 的 sw/noshm debug flags，wsi_common_x11.c 的 SHM pixmap 创建、DRI3 fence 导入以及无 SHM 时软件图像/呈现分支。关闭 SHM 后同一完整顺序两次全通过，表明故障与当前原生共享图像路径相关；本隔离会话与显示服务间的具体故障原因仍未证明。源码包 SHA256 `1628058a8d2c0615975de5a15ab7bbb9638c50000b5bed9456ff423ea034a81f`。未安装、更换或补丁修改 Mesa，也没有将 workaround 写入生产启动。原生共享图像路径继续开放，按用户要求等换回 4060 Ti 再验收。诊断用 GDB 包装器的汇总 PASS 不作为测试通过证据；只使用最终真实 CTest 和报告日志。
