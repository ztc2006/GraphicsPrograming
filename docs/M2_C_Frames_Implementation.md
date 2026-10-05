# M2-C1：帧上下文与完成回收

2026-10-04。图形代码前使用 `real-time-rendering-advisor`，比较一帧、两帧共享目标、复制目标和 timeline/多队列方案。选择及官方证据见 [决定](M2_C_Frames_Decision.md)。

`Renderer(Device, framesInFlight=1)` 启动时固定配置，只接受 1 或 2。`--frames-in-flight 1|2` 经 `run.sh` 传递，默认一帧；Render Debug 显示实际配置。动态改帧数或建立时间历史不在本增量。

每个槽位独立持有 uniform buffer（448 bytes）、descriptor set、command buffer、十项 timestamp query pool、acquire semaphore 和 submit fence。descriptor pool、command 分配和轮转均使用实际槽位数量。环境、材质、网格、HDR、主深度和阴影目标共享；pass 之间及不同提交之间通过图的 stage/access/layout 屏障依赖排序。两帧允许 CPU 录制下一提交，目标冲突仍会限制 GPU 重叠，尚不承诺提速或更低延迟。render-finished semaphore 继续按 swapchain image 分配。

## 生命周期

1. 复用槽位前等待 fence，交付旧 timestamp，再清除绑定这个 fence 的旧 image 关联。随后才写 uniform、reset command/query。
2. acquire 与录制不分配提交 ID。成功 queue submit 后分配单调 ID、登记 submitted 和 image fence，并发布图最终状态。
3. 完成 fence 后读取全部 query availability。缺失查询时拒绝复用，避免把旧查询静默重置。无 timestamp 支持时交付 `valid=false` 的完成事件。
4. 每个提交恰好交付一次完成回调，回调不抛异常且不重入。多个完成槽位逐项交付；最新 UI 计时只接受更高 ID。回收顺序可以与提交 ID 顺序不同。
5. 单 graphics queue 的较晚 fence 覆盖前序提交，完成水位可以退休资产及旧预览。退休不等于 WSI presentation 完成。
6. swapchain 目标替换先收集查询；存在 pending 帧时拒绝构建替换。Application resize/退出沿用 drain 路径，零尺寸等待新增关闭检查。

Application 的 benchmark 订阅逐帧完成回调，按 ID 回填采样行；暖机未采样帧忽略，结束时 drain 全部帧。报告升为 schema 4，新增 `frames_in_flight`、`swapchain_image_count` 与 `frame_target_policy=shared_hdr_depth_shadow`，保留 HDR、资源账本及其余计时字段。不再轮询一个会覆盖其他槽位的最新结果来生成完整报告。

## 验证方法

CPU 验证默认/两帧选项及非法数值、缺值，脚本转发及 schema/config。生产 WSI 的 HDR/图、异步场景、真实 ImGui 预览、取消、resize/restore 与退出在一帧、两帧各运行。所有最终 GPU 资源仍必须归零。

离屏测试使用两个生产槽位的 uniform/descriptor/command/query/fence、PBR shader 和 graph backend，省略 WSI acquire/present。已有 timeline semaphore 能力构成未来 host signal 闸门，阻塞两个已提交 GPU 绘制。metallic=.25 和 roughness=.75 的不同 debug uniform 应产生 FP16 RGB `.25` / `.75`，alpha=1；闸门关闭时无完成事件、无 UI 释放、旧场景保活。放行后一次收集交付两条查询，重复收集不重复交付，旧场景/UI 恰好退休一次。第二轮逆序使用槽位，收集顺序为 ID 4 后 ID 3，UI/完成水位必须保持 4；再次回读正确、无陈旧 fence，退出账本归零。闸门只用于测试，避免某些驱动的 host present 阻塞测试信号的发出。

## 尚未关闭的条件

4060 Ti 吞吐/输入延迟/帧预算/显存、validation 与新 RenderDoc 抓帧等用户换机通知。软件测试不作为这些验收的替代。当前 WM 未确认 native iconify，显式恢复/重建通过，原生零尺寸等待需补现场测试。

下一项 M2-C2 是 WSI presentation 生命周期：评估并接入可用的 swapchain maintenance/present fence 与清晰回退。官方指南指出 submit fence 和未扩展 WaitIdle 不能提供完整的 presentation 资源释放证明；本增量保留当前退路，不宣称已解决。M2 生命周期闭合后推进 M3 typed mip、材质与完整 IBL。


## 本轮结果

Release/Ninja 构建成功，无新编译诊断；13/13 CTest 通过（9 CPU + 4 llvmpipe/X11 GPU）。一帧/两帧均通过生产 HDR/graph 数值回读、异步加载/取消/预览退休、三次 resize、实际 ImGui restore/recreation 和退出归零。timeline 闸门测试真实执行不同 uniform 的 shader，FP16 RGB 回读 `.25` / `.75`、alpha=1；pending 时旧场景/UI 保活，完成后两条查询交付且退休一次，重复收集不重复；再次使用槽位按 ID 4/3 收集，最新 UI/水位保持 4。原 4 commits / 6 frames / 5 uploads / 6 image copies / 0 runtime upload waits 合同保留。

报告烟测一帧 30 帧、两帧 24 帧，所有 query 与行匹配；软件帧数波动不作为配置优劣结论。四合院一帧/两帧各 5 帧/query，原生 1920×1080/UI/orbit/FIFO，正常退出。三个 swapchain images 独立于 frame count。两帧较一帧仅增加一个 uniform/descriptor set：payload/suballocation +448 bytes，buffers 84→85，sets 29→30；images/views 保持 53，allocator backing 保持 17 blocks / 302,722,140 bytes。当前 payload 为 295,481,120 / 295,481,568 bytes。已验证软件资源占用，不代表硬件显存/延迟预算。

源码 SHA256 `e9ebba375125bfd1785dc18acb9105d4375654ec36c8410de834aae4b20c448e` 与构建头和两个报告一致。测试、报告、验证 JSON 和任务增量补丁位于聊天工作区 `outputs`。本增量与 M2-B 至 M3-C1 纳入同一次集成提交；各阶段原始验证记录保留。

后续记录：M2-C2 的扩展呈现资源所有权/drain 已接入，软件 WSI 验证和 legacy/原生路径边界见 [呈现合同](M2_C2_Presentation_Implementation.md)。下一实现入口已移至 M3-A。
