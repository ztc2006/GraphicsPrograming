# M3-B 环境镜面反射决策（2026-10-04）

## 约束与证据

沿用 Linux / RTX 4060 Ti / 原生 1080p 60 FPS 的路线。环境图目前只在 Renderer 冷启动载入，场景重载不应重建环境资源；动态局部探针是 M8。物理硬件验收按用户要求延后，本阶段的软件结果不代表 GPU 性能或最终画质验收。

已先使用 real-time-rendering-advisor。RTR4 第 9 章的微表面 BRDF、10.5/10.6 的环境采样与预过滤、球谐（SH）是稳定基线；本地中文 PDF 页 332/347/350 核对了立体角、预过滤重要性采样及掠射角近似限制。公式采用 [Karis 2013, Epic 官方讲义](https://cdn2.unrealengine.com/Resources/files/2013SiggraphPresentationsNotes-26915738.pdf) 的 GGX、Hammersley、cos 加权预过滤和 split-sum A/B 积分。生产实现交叉核对 [Filament 官方文档](https://google.github.io/filament/Filament.html#lighting/imagebasedlights) 的 roughness mip、DFG LUT、线性辐亮度及 SH 分工（2026-10-04）；这里使用 Karis 的单次散射/Schlick-Smith IBL 可见性，不能混用 Filament 的相关 Smith LUT 或多次散射能量补偿。agent-reach 的 Jina 后端取得 Filament 正文；Vulkan 文档网页被 403 拦截、本机 gh 不存在，遂以 web 读取 Khronos 官方原始章节并核对面选择表。Vulkan cube 面和层的契约依据 [Khronos 纹理规范](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/chapters/textures.adoc)。

## 选择

| 方案 | 优势 | 代价/适用边界 |
|---|---|---|
| CPU 确定性烘焙 + 磁盘缓存（本次） | 脱离驱动验证公式；缓存命中避免重复积分；复用上传批次 | 首次启动耗 CPU 时间；不适合频繁刷新探针 |
| GPU compute 烘焙 | 更新动态探针时并行工作 | 新增管线、存储资源、同步及目标驱动验收；待 M8 更新需求和实测 |
| 独立离线工具 | 完全移除启动积分 | 部署/资产流水线更复杂；后续复用相同烘焙接口 |

本次实现 CPU 烘焙与可选磁盘缓存，冷启动一次批量上传；不改变后台模型准备、弱纹理共享、主线程提交/就绪和帧退休链路，也不加入运行时环境切换。缓存失败/不可写不阻止渲染。

## 数据、采样和精度契约（先于代码）

输入是已翻转为 v 向上约定的 equirectangular 线性 HDR，u=(atan2(z,x)/2π)+.5、v=(asin(y)/π)+.5，水平重复、垂直夹边。RGB 必须非负有限，维度和数量严格检查。先用 2×2 分层采样转换为 cube（尺寸由源尺寸/输出档位决定，上限 1024，规则纳入版本），生成按准确 cube 纹素立体角加权的源 mip 链并跨面双线性采样，预过滤样本按 GGX PDF 与输出纹素中较大的立体角选源 LOD，避免直接点取高亮小光源。经纬图按面积选各向同性 mip 的试验在极点方向梯度上失败，已取消该表示。cube 转换、FIS 与低分辨率 mip 此过滤引入可测量偏差，不保证任意高频环境的精确积分。

输出 cube 使用 Vulkan +X,-X,+Y,-Y,+Z,-Z 六层。面内 s/t 在 [-1,1]：(+1,-t,-s)、(-1,-t,+s)、(+s,+1,+t)、(+s,-1,-t)、(+s,-t,+1)、(-s,-t,-1)，归一化后采样。LOD0 按 cube 纹素立体角选择源 mip 后重采样，避免缩小环境图直接点采样；后续 mip 每级独立卷积，N=V=R，r=mip/(mipCount-1)，α=r²，归一化 sum(Li·NoL)/sum(NoL)。默认 face=128、512 样本、8 mip；高亮/缝隙与运动验收后可调整档位。

BRDF LUT 默认 128²、1024 样本，横轴 NoV、纵轴感知粗糙度 r，中心采样；A/B=(1-Fc,Fc)·G·VoH/(NoH·NoV)，Fc=(1-VoH)^5，IBL G1 用 k=α/2（不可把直接光 k=(r+1)²/8 搬入 LUT）。着色器反射=textureLod(cube,R,r·(levels-1))·(F0·A+B)，不再 roughness 混合 SH，也不再重复乘 roughness Fresnel。保留 SH 漫反射 irradiance/π，并校准常量环境与方向梯度。SH 投影使用准确纬度立体角及高精度累加。

CPU 用 float HDR 存储、double 累加。GPU 先用 RGBA32F cube（约 2 MiB）和 RGBA32F LUT（约 .25 MiB，RG 有效）；保留原 2D 天空贴图。暂不引入半精度量化/压缩，之后由误差和带宽测量决定。要求 sampled/linear filtering 格式特性和 cube 维度上限。曝光、环境强度、旋转、AO 不烘入缓存，在帧参数/输出阶段处理。

上传资源：cube-compatible 2D image、6 层完整 mip 视图、clamp/trilinear/no-anisotropy sampler；LUT clamp/linear/no-mip。一个 staging payload 按 mip→face 紧密排列，copy 区域包含全部层；barrier 覆盖全部 mip/层，UNDEFINED→TRANSFER_DST→SHADER_READ，transfer write→fragment sampled read，同图形队列。冷启动可等待一次；模型切换不得增加运行时上传 fence wait。描述符增加 cube/LUT，UBO 448 字节不变，通过 textureQueryLevels 获取最大 LOD。资源租约必须在 GPU 对象释放后归零。

缓存身份包含源维度/完整 float 位模式、算法/格式版本、face size、prefilter 样本、LUT size/样本及固定方向/BRDF约定版本；哈希仅定位，读取必须比对完整身份。文件使用版本化显式整数/浮点编码、长度和完整性校验；缺失、陈旧、截断、损坏都重新烘焙，临时文件原子替换防止半文件。缓存不持有 GPU 对象。

## 可证伪验收

- CPU 常量 HDR >1：每面/每 mip 辐亮度误差 <1e-4 相对；SH irradiance≈πLi（含低分辨率）；不夹到 1、不夹曝光。
- 面中心/边界方向渐变：轴符号、水平缝、上下极点正确；GPU samplerCube 实际读取验证。
- LUT 粗糙度/NoV 网格 finite/nonnegative，白炉 A+B≤1 加离散误差；r=0 Schlick 极限、r=1/NoV=1 的 1-ln2 独立解析基准；额外用半球积分核对非端点。
- 高频/小亮源：粗糙层扩大亮域且降低峰值，没有 NaN、黑缝或常量能量漂移。
- 缓存重复命中位一致；改变源/任一参数失效；损坏/截断/不可写回退；并发写不暴露半文件。
- 软件 Vulkan 读取六面/各 mip/LUT 与生产 PBR 输出；曝光只改显示不改 scene HDR，环境强度线性、旋转一致。原有一/两帧、场景事务、缓存复用/退休、UI/重载/退出全套通过，无跳过，最终账本零。
- 4060 Ti 的 pass 时间、原生 WSI、运动缝隙/高光稳定、最终金属与室内材质画质及 RenderDoc 继续延后。split-sum 的视角各向同性、单次散射高粗糙度能量损失、SH 截断和全局天空不能表达局部遮挡是明确边界；局部反射 M8。
