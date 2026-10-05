# M2-C2：呈现资源生命周期决定

2026-10-04。图形代码前使用 real-time-rendering-advisor decision/pipeline 参考；RTR4 第 2–3/23 章用于执行/生命周期模型，第 18 章用于测量。Vulkan 现行 WSI 规则是后续官方证据。目标 Linux、单 graphics queue、固定原生 1080p、默认一帧/可选两帧与共享 HDR/depth/shadow 不变；4060 Ti 验收等待用户通知。

选择可用时启用 KHR/EXT swapchain maintenance 的呈现 fence（presentation fence），把按 swapchain image 的 render-finished semaphore 和 present fence 交给 SwapChain。Renderer 只借用信号量并提交绘制，再交给 SwapChain 呈现；场景/UI 仍按 submit ID/fence 退休。present fence 表示呈现资源可释放，不是屏幕 scanout 时间，不能用于声称输入延迟或已显示。

| 候选 | 决定 |
|---|---|
| 仅 WaitIdle | 保留作 legacy 回退；官方指南不提供完整 presentation 释放证明，报告不得把它标为 proven |
| maintenance present fence | 选定；可直接等待每代全部已入队请求，当前范围只处理释放，不加动态 present mode/scaling |
| acquire fence + 保留退休 swapchain | 能间接验证 semaphore 复用，但最终退出仍缺证明，历史代链复杂；当前不采纳 |
| present-id/present-wait | 用于显示/节奏测量，超出本轮资源回收，不同时引入 |

实例只添加实际支持的 get_surface_capabilities2 与匹配 KHR/EXT surface maintenance 依赖，可以同时启用两族来兼容设备选择。设备只在匹配实例依赖、device extension 和 swapchainMaintenance1 feature 均满足时启用对应 device extension/feature，优先 KHR。auto 自动选择，fence 要求支持否则启动失败，legacy 明确关闭扩展以验证退路。不改变物理设备选择优先级。

SwapChain 每个图像持有 semaphore，扩展路径另持有一个 fence。复用 fence 前等待上次已入队请求、交付完成计数，再 reset；新请求成功/suboptimal 或 WSI 拒绝类错误（包括 out-of-date/surface-lost）仍标为已入队并最终 drain。OOM 拒绝入队时不等待未提交 fence。device-lost/未知错误诊断后走终止处理，不能无限等一个状态未知的 fence，也不能声称释放已证明。重建/退出先完成 GPU 提交与呈现资源 drain，之后释放旧 views/semaphores/fences/swapchain；generation 所有权不再随 Renderer pipeline 重建而更换。

报告与 UI 显示 backend/原因及释放证明状态，保留 schema 4 原字段。验证 CPU 的依赖/feature/策略/结果分类；软件 WSI 一/两帧、KHR/EXT 可用路径与强制 legacy、每图像 fence 复用、pending 请求与幂等 drain、resize 与 acquire out-of-date 恢复、Renderer 重建不替换 present semaphore、未入队 OOM 不等 fence、正常退出与资源归零。真实 WSI 不可稳定制造的错误采用纯状态合同测试，不冒充驱动测试。原生最小化/零尺寸、validation、新 RenderDoc 和硬件预算依旧开放。

官方来源核对于 2026-10-04：[present fence 释放合同](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html)、[queue present 结果语义](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html)、[KHR swapchain maintenance](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_swapchain_maintenance1.html)、[KHR surface 依赖](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_surface_maintenance1.html)、[EXT 兼容族](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_swapchain_maintenance1.html)、[semaphore/WaitIdle 边界](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)。Jina 只返回 security verification 页面，已通过官方 web 阅读回退获取正文；该 challenge 不作为证据。
