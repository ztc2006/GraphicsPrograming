# M3-C2：法线过滤与镜面抗锯齿决定

2026-10-05。写图形代码前使用 real-time-rendering-advisor；Linux/Vulkan 1.3、原生 1080p/60 FPS、静态 glTF/OBJ、运动稳定性约束已确认。4060 Ti 实测按用户要求延后。

采用方差映射（Variance Mapping）与几何镜面抗锯齿（Geometric Specular Antialiasing）的有界组合，保持既有 GGX 与单次散射 A/B LUT。纹理数据和几何变化分别估计；这是各向同性粗糙度扩宽近似，GGX 没有有限统计方差，不能称为精确卷积或能量恢复。

| 方案 | 取舍 |
|---|---|
| 原始 Toksvig，RGB 存未归一化均值 | 可响应硬件插值，但 RGBA8 小向量方向精度差、破坏现有单位 mip 合同；本次不用 |
| 法线 alpha 存均值长度损失 | 无新 GPU 存储/采样器，独立 normal/MR UV 可用，LOD0 RGB 不变；不包含 mip 内双线性过滤引入的方差，采用此方案 |
| LEAN/CLEAN/斜率矩 | 可表达更丰富分布，但增加精度/存储/材质合同，当前 16 采样器已满；后置 |
| 每光源半向量导数 | 方向相关更精确，多光源成本增加；先用与光源无关的几何法线导数近似 |
| TAA/超采样 | 必须补运动、边缘与历史处理，不能代替材料过滤；TAA 保持 M6 |

API 中立数据流：解码法线 RGB → 单位 LOD0 法线 → 面积加权未归一化一阶矩逐级传播 → 每个输出 mip 归一化方向及长度损失 d=1-|mean| → RGBA8 原通道上传。只在 Normal policy 中 alpha 被内部元数据接管：LOD0 A=0，其 RGB 逐字节保留；其他类型的 A/coverage 不变。不支持把 normal A 同时作为自定义其他材质语义。退化均值用 +Z、d=1；连续平面 d=0。算法/cache version 2→3，原始编码、空间、policy、sampler 隔离继续存在。

生成时不从已量化/归一化的输出回推下级。第一层从原图逐 texel 归一化，后续使用 float3 一阶矩工作层；只保留相邻两层，常见方形峰值为 12*(N/4+N/16)=3.75N 字节的额外 CPU 临时量，1D/奇数尺寸按实际层大小记。均值求和用 double，存工作层 float；GPU RGB 与 d 的量化步长 1/255，d 误差≤0.5/255，平面 d=0 精确。接近完全抵消时有界核饱和；量化会漏掉非常小的方差，不能声称消除全部细小闪烁。

着色参数：感知粗糙度 r 保留 [0.04,1]，GGX alpha=r²。纹理 sigma²≈d/max(1-d,1/255)，纹理核 kt=min(2*sigma²*normalScale²,1)。normalScale² 是小斜率近似，0 时精确关闭、负值对称，强倾斜/各向异性误差需质量验收。几何核 kg=min(2*0.15*(|dN/dx|²+|dN/dy|²),0.2)，使用贴图扰动之前的单位世界法线，避免重复计算纹理变化。在 discard/POM 分歧前取几何导数及 normal UV 梯度；normal sample 使用 textureGrad，沿用 POM 的 UV 偏移、梯度近似为原 UV 足迹。高度推导法线仅获得几何过滤，不假称含高度方差。

过滤 r'=sqrt(sqrt(min(1,r⁴+kt+kg)))，没有额外 Fresnel 倍乘；直接 D/G、IBL cube LOD/LUT 与现有粗糙度 Schlick 一起使用 r'，预烘焙 BRDF 模型/版本不变。复用空闲 frame lightingParams.z 为 AA 开关，删未使用 shininess；UBO 448B、push112B、Material48B、Vertex112B、combined samplers16 都不增加。开关写每帧槽 UBO，不改在途材质描述符。新增 authored roughness、texture kernel、geometry kernel debug，已有 Roughness 显示 r'。无新 pass/barrier/图资源/上传等待。

验收：独立 CPU 对称/混合分布、平面/抵消/多级/NPOT/alpha/version；实际 GPU 全 mip 回读和插值；生产像素开关 A/B、r 下限/饱和、频率/LOD、scale0/负值/非单位、镜像/非均匀变换、dielectric/metal、HDR 直接光及 IBL/LUT；旧 alpha/POM/specular 和一/两帧事务、reload/cancel/rollback/retirement/最终归零全套继续通过。粗糙度像素误差≤0.002；HDR参考误差取≤max(0.012,0.005*期望)，几何导数用独立差分参考。四合院同设置账本 GPU payload/对象数预期零增量（分配块实际实测）；CPU 临时内存不属于 GPU账本。运动对比、帧预算、真实 VRAM/validation/RenderDoc 等 4060 Ti，不以软件短跑帧率代替。

证据：RTR4 第9章法线分布过滤（本地 PDF 307–310页，特别309页说明 Toksvig/LEAN/variance mapping/导数以及过滤遗漏）。[Stephen Hill 原文](https://blog.selfshadow.com/2011/07/22/specular-showdown/) 给出 (1-L)/L 与预计算取舍；[Tokuyoshi/Kaplanyan I3D 2019 原论文](https://www.jp.square-enix.com/tech/library/pdf/ImprovedGeometricSpecularAA.pdf) Listing2 给出保守各向同性导数核；[Filament 官方实现](https://github.com/google/filament/blob/main/shaders/src/surface_shading_lit.fs) 明确 forward 使用此近似，[材质文档](https://google.github.io/filament/Materials.md.html) 参数默认0.15/0.2。2026-10-05核对官方源码/资料；没有引入论文的每光源 forward 精确版本，没有把工程近似冒称精确 GGX 方差。
