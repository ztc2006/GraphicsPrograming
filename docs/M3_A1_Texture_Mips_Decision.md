# M3-A1：纹理缩小过滤决定

日期：2026-10-04。实现前使用 real-time-rendering-advisor 的 texturing/PBR 与 decision playbook；这是 M3-A 的第一增量。

采用后台 CPU typed mip，再通过原有单批上传完整链。Linux Vulkan、原生 1080p/60 FPS、静态 glTF/GLB、OBJ 兼容、运动稳定性是已确认约束。4060 Ti 现场验收等待用户换机通知，本轮只用显式 llvmpipe/X11 软件环境。

## 比较和范围

| 方案 | 画质/语义 | 加载/资源代价 | 决定 |
| --- | --- | --- | --- |
| CPU 按用途过滤 | 可直接验证 sRGB、线性通道、法线和奇数尺寸 | 后台计算与约 4/3 的方形 texel payload；长条图最高接近 2 倍；上传更多数据 | 本轮采用 |
| GPU blit | 普通颜色可行；法线和 mask 需要额外专用步骤 | 需 format blit/filter 能力、逐层 barrier；普通 blit 不能解决全部语义 | 当前不采用 |
| GPU compute/离线缓存 | 适合后续压缩/大资产、专用过滤和缓存 | 引入计算管线/存储格式或缓存失效合同 | 加载测量证明需要后再扩展 |

颜色 RGB 使用精确 sRGB EOTF 解码，面积 box 过滤后重新编码；alpha 始终是线性、非预乘。线性颜色和 packed MR/AO/height/alpha 逐通道平均，不对 roughness 宣称 BRDF 能量补偿。法线解码到 [-1,1] 后平均并归一化，近零向量回退 +Z；不会保存法线方差，镜面抗锯齿仍开放。每级尺寸 floor/2，面积权重覆盖奇数尺寸的末行/末列。

alpha mask 的 baseColor 和独立 alpha 图像使用显式 BaseOnly 策略；A2 做 cutoff/factor 与覆盖率后再启用它们的完整 mip。BLEND 保持 straight alpha；透明边缘颜色扩张/过滤仍需专门验收。HDR 环境保持单层，普通 mip 不能替代 GGX roughness 预过滤。

## 数据、同步和身份

解码 → typed CPU 链（RGBA8，float 过滤）→ 单图像全 mip 创建/view → 一份 staging、多个 copy region → 全链 transfer write 到 fragment sampled read barrier。仍在同一图形队列上传，不新增运行时等待；上传 ticket、取消保活和完成帧退休保持原有合同。资源账本按完整 texel payload 计数，VMA backing/range 独立记录。

图像缓存键包含原始内容、颜色空间、过滤用途、Generate/BaseOnly 和算法版本；sampler 单独共享。未来 alpha coverage 的 cutoff/factor 必须进入键，不能别名到当前链。

glTF 既有六种 min、两种 mag 和 wrap 显式值保留。省略 filter 时采用线性/trilinear 引擎策略。mip sampler 使用 VK_LOD_CLAMP_NONE，由 view 限制实际级数；非 mip 模式 nearest mip + maxLod=0.25 保留 min/mag 区别。8x 各向异性只对全线性/trilinear 请求启用，受已启用 device feature/limit 限制，可显式关闭；nearest/非 mip 请求保持精确过滤。缓存键包含该请求，设备能力在 cache 生命周期内固定。

## 可否证验收

CPU 检查黑白 sRGB mip≈188、线性数据≈128、alpha≈128、法线单位长度/零向量、3×5/1×N 全覆盖、链尺寸/偏移/溢出和 BaseOnly。真实软件 Vulkan 通过生产 loader/cache/upload 创建图像：全层回读、显式 LOD raster 采样验证 view、trilinear/nearest、非 mip min/mag、wrap 和共享身份；账本归零。既有异步/取消/退休、一/两帧和呈现测试全部保持。四合院运行报告记录新内存，不用软件帧时宣称 60 FPS。

换机后补 RenderDoc 全链/采样器检查、屋瓦/斜地面运动 A/B、加载 CPU/峰值内存与 1080p GPU 成本。A1 不关闭 alpha coverage、tangent/normal 材质合同、specular AA 或完整 IBL。

## 依据边界

RTR4 第 6 章纹理、第 5 章颜色/透明、第 9 章材质是稳定模型依据；本轮未引入后 RTR4 研究算法。

2026-10-04 核对 [Khronos glTF 2.0 规范](https://github.com/KhronosGroup/glTF/blob/main/specification/2.0/Specification.adoc)：sampler、颜色/数据和 straight alpha 语义。省略 filter 的 trilinear 是引擎选择。核对 [Vulkan VkSamplerCreateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkSamplerCreateInfo.html)：非 mip 0.25 映射、LOD clamp 与 feature/limit 约束。CPU 法线过滤及 mask BaseOnly 是本项目的阶段选择。Jina 读取遇到网页验证，使用官方源文和文档回退。
