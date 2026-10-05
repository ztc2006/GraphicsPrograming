# M2-B：单队列 Render Graph 决定

2026-10-04。开始图形代码前参考 `real-time-rendering-advisor` 的 decision-playbooks、pipeline/GPU/transform，以及 `codebase-design` 的深模块接口原则。RTR4 第 2–3 章提供阶段/执行模型，第 18 章提供测量基线，第 23 章提供同步和带宽取舍；Vulkan 1.3 的具体屏障规则按官方当前文档核对。约束沿用已确认 Linux / 原生 1080p / RTX 4060 Ti / 静态场景与移动相机灯光；硬件验收仍延后。

选择范围受控的图：外部图像导入、单图形队列、整图单 mip/layer、固定声明顺序的读写语义与稳定拓扑排序、显式额外依赖、同步与 attachment 合同检查。图不分配 GPU 资源，Renderer 继续拥有帧和资产资源；编译生成的计划持有非拥有资源描述和局部状态，仅提交成功后发布下一帧状态。

| 方案 | 取舍 |
|---|---|
| 继续分散手工 barrier | 当前重复状态和 pass 调用顺序会扩散到 AO/TAA/SSR；无法统一验证初始化、保留与同布局写入依赖 |
| 最小声明图（选择） | 将资源语义、依赖检查、屏障和可检查计划集中，CPU 编译成本可测；第一轮没有自动资源复用 |
| 通用多队列/别名/优化图 | 当前需求不支持增加队列所有权、别名生命周期和多帧历史复杂度，延后测量后再评估 |

帧数据：阴影（必要时首次初始化）→ HDR 场景 + 主深度 → display → UI → present export。HDR/深度/阴影/swapchain 状态由图统一生成，HdrOutput 只负责资源与 fullscreen draw。主深度使用 sampled/transfer-src 能力检查和 STORE，并在帧末导出为 depth-read-only。纹理和上传仍由已有资产路径管理，不把所有资源管理扩张进本轮图。

Application 提交可见 opaque/mask/blend 与完整 opaque/mask 列表、相机、灯光和 debug 设置；Renderer 决定 pass。现有逐项录制接口保留为同一执行路径的兼容入口，先收集 draw，再执行图；不允许它安排 GPU pass。透明顺序保留。所有图定义/资源和高层 draw 输入错误尽早拒绝。

读写依赖按声明序推导 RAW/WAR/WAW；额外依赖可排列独立 pass，循环拒绝。CLEAR 初始化，LOAD 要求已定义内容；DONT_CARE 的全覆盖必须显式声明；STORE DONT_CARE 后不得采样或 LOAD。同布局的写入依赖也生成 barrier；连续只读合并作用域。单队列导入和最终导出使用明确 stage/access/layout。swapchain 的 acquire/present semaphore 和已有共享队列族策略保留。

UI 独立 rendering，display STORE → color read/write barrier → UI LOAD/STORE，随后 present。额外 attachment 存取的实际成本必须在硬件上测量，不承诺这次架构迁移加速。初次关闭阴影时也初始化一次 far-depth，避免未定义 descriptor 内容；该初始化使用图和真实 shadow 时间区间。之后关闭阴影复用已定义的只读资源。

验收：纯编译测试的错误输入、读前写/丢弃后读、无覆盖写、同布局 WAW、累积读作用域、依赖顺序与循环、最终导出；生产 shader 的 HDR/透明/天空/显示回读不变，新增主深度和 UI 保留回读、阴影开关和跨帧状态测试。保留真实异步 ImGui、共享/失败/取消/退休、resize/归零、完整报告与四合院烟测。输出 pass/resource/barrier dump。软件结果只验证正确性，4060 Ti timing/validation/运动画质仍后置。

官方语义核对（2026-10-04）：[同步示例](https://docs.vulkan.org/guide/latest/synchronization_examples.html)、[同步规范](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)、[动态渲染和附件 load/store](https://docs.vulkan.org/spec/latest/chapters/renderpass.html)。Jina 读取返回 403，回退官方网页读取；不将访问失败视为内容证据。采用 synchronization2，深度写作用域同时覆盖 early/late tests，颜色写作用域为 color-attachment-output。
