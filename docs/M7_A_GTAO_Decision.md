# M7-A：GTAO 基础通路决策

2026-10-08。实施前核对 real-time-rendering-advisor、RTR4 第11–12章（AO/间接光和图像空间方法），以及 [Jimenez 等 GTAO 原始策略](https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf) 和 [XeGTAO 作者说明与积分实现](https://github.com/GameTechDev/XeGTAO/blob/master/Source/Rendering/Shaders/XeGTAO.hlsli)。本轮实际读到作者 README/源码；原始 PDF 在线解析器因文件大小拒绝，公式以作者实现逐项核对，不编造书页。XeGTAO 已归档，只作公开算法参考，不引入 Vulkan 外部依赖。

目标仍为 RTX4060Ti、1080p/60FPS、单 graphics queue、1/2槽、动态相机和刚体、PBR/alpha mask/blend。既有厨房、四合院、FlightHelmet 为代表资产。AO 是局部可见性近似，不是动态GI。

## 比较与范围

采样核 SSAO 简单但方向积分/尺度误差较大；完整 XeGTAO 的深度 mip/compute/边缘压缩更复杂；RT AO 需要新的加速结构与降噪。先实施自己的有界 GTAO：3个切片、每侧6个距离、解析余弦积分、距离衰减、深度重建法线、5x5空间滤波。样本采用固定像素噪声，暂不增加独立时间AO历史；最终HDR走现有TAA。原生分辨率、最大采样半径128px，避免极近处无界跨度；该限制意味着大半径近处覆盖缩短，不能宣称完整XeGTAO质量或其第三方耗时。

## 数据与同步

主通道增加 RGBA16F 间接漫反射 MRT（材质AO已包含）；天空/debug/unlit/透明自身输出零，透明混合按alpha衰减底层间接漫反射，MASK复用原discard。没有额外几何prepass。随后读取 D32 深度生成 R16F raw AO，读取深度/raw 做 R16F空间滤波，读取HDR/间接漫反射/filtered生成RGBA16F composite：`HDR - diffuse * (1 - AO)`。仅漫反射环境光被衰减，直接光、自发光和镜面保留；透明不作为遮挡体，遮挡只来自不透明/MASK深度。

RenderGraph声明所有attachment→sample RAW及跨帧WAR/WAW，四个新目标共享单队列串行访问；描述符初始化后不改写，参数存入command push，每槽独立query，完成后才回收/交付。状态仅成功submit后发布；resize先排空旧用户并完整构造替换。新增20B/pixel载荷，1080p41,472,000B，不等于driver heap。关闭AO跳过三pass，但目标/MRT仍存在，A/B不包含这部分固定开销。主UBO608B/push128B不改，独立AO push112B/最多4samplers。探针捕获增加独立临时第三attachment，仍不运行AO，避免把AO烘进probe又乘一次。

## 验证

CPU检查非法/非有限参数与CLI/report语义；实际GPU检查无遮挡平面/天空≈1、凹角接触下降、屏幕边缘不重复夹边、有限范围、大半径上限、空间滤波范围、直射/自发光隔离、透明和MASK；AO开关/参数变化清TAA，未变化设置保留。检查1/2帧、pending资源/queries、resize、probe、最终ledger归零。导出厨房同曝光/相机AO off/on/debug HDR及可视图片；固定慢/快相机片段留作运动质量证据。RTX短测分别记录AO三pass和total/主通道，保留慢acquire；短测不替代30s暖机+3x120s最终协议。validation layer不可用时明确记录。

本轮交付基础AO通路；参考质量调优、独立AO时间重用、半分辨率/深度mip、Bloom/自动曝光留给后续增量；BVH/SDF按用户顺序仍在AO之后。
