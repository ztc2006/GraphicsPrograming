# M2-C1：可配置帧上下文与完成回收决定

2026-10-04。图形代码前使用 `real-time-rendering-advisor` 的 decision/pipeline/performance 参考。RTR4 第 2–3/23 章用于执行和吞吐/延迟模型，第 18 章用于测量。Linux、原生 1080p、单 graphics queue、静态资产与移动相机/灯光约束不变；4060 Ti 硬件验收等待用户换机。

选择启动时固定的一帧/两帧配置，默认一帧。帧独立 uniform、descriptor、command buffer、query pool、acquire semaphore 和 submit fence；共享 HDR/depth/shadow 目标继续通过图的跨提交读写屏障串行访问。场景资产不复制，输出完成信号量仍按 swapchain image 配置。帧提交编号仅在成功 submit 后分配。

| 方案 | 取舍 |
|---|---|
| 只保留一帧 | 已验证且延迟/资源简单，无法比较 CPU 录制与 GPU 执行重叠 |
| 一/两帧 + 共享目标（选定） | 修改独立帧资源和完成回收，目标内存不翻倍；GPU 目标冲突仍有依赖，不承诺 GPU pass 并行或加速 |
| 每帧复制 HDR/depth/shadow | 减少某些跨帧目标依赖，但 1080p 每组约 39.7 MiB，且输出 descriptor/pipeline 职责需再拆；测量显示目标依赖限制吞吐时再比较 |
| 改用全局 timeline/多队列 | 当前两个槽位用 fence 足够；避免同时改变生产上传和 WSI 同步 |

每次重用槽位前等其 fence，完整交付旧 query，然后才能改 uniform、reset command/query。query 未完整可用时拒绝重用。成功提交后登记 ID、图的最终状态和该图像的 fence；收集完成时清除该 fence 的图像关联，防止复用 handle 造成陈旧关联。单队列后一次提交的 fence 覆盖之前提交，完成水位可用于退休旧资产和 UI descriptor；所有完成样本逐项回调交付，UI 保留最高 ID，不再依赖一个容易覆盖的计时值。

Application 在 benchmark 时订阅完成样本，按 ID 回填行；终止测量先 drain 全部提交。报告明确记录 frames-in-flight、swapchain image count 与共享目标策略。CLI/Renderer 拒绝一、两帧之外的配置。resize/关闭沿用当前 WSI 路径，重建前 drain 帧结果；最小化零尺寸等待检查退出。

验证分两层：真实 WSI 一/两帧均跑生产 HDR/图/异步 ImGui 资产/resize/归零；离屏用已有 timeline semaphore 测试闸门，阻塞两个真实 frame-slot 的 GPU 绘制，验证不同 uniform 产生不同 FP16 像素、完成前不退休、一次收集交付两个查询、重新收集不重复、槽位复用和下一场景正常。闸门只在测试使用，避免驱动的 present 调用阻塞 host 导致无法构建可重复的 pending 两帧条件。

证据与边界（2026-10-04）：[Vulkan 帧配置教程](https://docs.vulkan.org/tutorial/latest/03_Drawing_a_triangle/03_Drawing/03_Frames_in_flight.html)、[提交 fence 和屏障作用域](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)、[swapchain semaphore 复用指南](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)。Jina 返回 403，已回退官方页面验证。按图像的 present-wait semaphore 保留；submit fence 不能证明 presentation engine 完成。现有未扩展 WSI 的 WaitIdle 销毁模式无法提供完整的 presentation 资源释放证明，M2-C2 应接入可用的 swapchain maintenance/present fence 与回退诊断，之后才关闭 M2 的生命周期条目。本增量不宣称这一项或原生 iconify 已验收。

软件测试仅验证上述条件。帧预算、排队延迟、图同步成本和是否应默认两帧必须在 4060 Ti 固定场景/配置与重复测量后决定。M3 材质、mip 与完整 IBL 在 M2 生命周期合同之后推进。
