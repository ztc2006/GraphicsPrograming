# RT-B 光照与光传输实施记录

2026-10-09。独立RT Pipeline现已接入太阳/点/聚光灯、真实shadow visibility、面积/环境采样、GGX反射、多次漫反弹和静止累积。UI仍仅切换Raster/Ray tracing，增加当前样本数只读信息。已安装到用户原项目并完成原目录构建/烟测，见末尾。

## 结果和边界

raygen迭代路径，递归深度1，初始1 spp/帧、最多8次表面事件；没有加入NRD或物理玻璃。透明仍是随机coverage近似，玻璃/薄片光学RT-C、成熟降噪RT-D继续按已确认方案推进。用户保留画质判断，此处仅工程验证与数据。

共享MaterialGpuStore的albedo/alpha/normal/MR/emissive/specular/specularColor、UV/sampler/颜色空间与因子；直接光GGX/Fresnel/G项提取为pbr_shared.glsl且原光栅数值保持。路径方向采用归一化GGX分布，现有前向D项保护/IBL不同近似是记录中的边界；不将其保护下限当作概率密度。LOD0、几何导数AA/POM缺口明确，未偷偷提升一条路径的材质数据。

实例mask1/2/4分别为相机/二次/投影者；光卡保持不堵窗但可出现在发光采样/二次命中，BLEND不投影。常规emissive也被采样，单/双面发光独立判断；光源实际三角形/世界面积和发光贴图参与NEE，power MIS与命中源/环境的互补权重避免双计，第三次事件后RR。按几何法线偏移，有限光源终点距离相对偏移后的起点计算，防止灯面自身遮蔽。

既有probeSH/ambient常数、AO与预滤间接光不叠加到真实路径；只读取全局环境cube的原始mip0。直达与环境的艺术强度分别应用其源项。有限深度截断和raw噪声是当前阶段边界，不以firefly clamp掩盖误差。

## 资源和历史

原608B main UBO、128B raster push、1088B indoor及112B Vertex保持；RT Push96B保持，新GPU实例144B、lighting header96B、area record80B。512纹理array按view+sampler去重并全部填充；256材质/512不同贴图超限仅使RT不可选，保留光栅。slot-local光/面积/参数缓冲，失败候选/提交与资源退休继续沿用原合同。

CPU为发光表保留位置12B/顶点和索引4B/条的轻量缓存（厨房逻辑payload约27.1MB，不算入GPU账本，std容器元数据另计）。BLAS/TLAS仍复用RT-A；TLAS每帧重建，静态复用/update尚未优化。新增黑色fallback cube只用于没有提供环境的底层调用。

独立RGBA32F目标同时存运行均值；图新增storage read/write与undefined拒绝，保留最后RT writer的同步来源。history key显式序列化view/sampler/layout，避免padding；相机/实例/材质/灯/环境/设置变化失效。计数与key只在成功submit后发布；EV仅影响显示，不进入scene-linear key。可显式固定随机种子做独立参考；生产默认连续序列。上限1,048,576样本后为固定权重更新，非无限精度累积。

## 验证

RTX4060Ti、NVIDIA615.71.09、原生X11/KHR：**45/45，73.56s**（20 CPU、25 GPU）。Shader五模块spirv-val通过。新增GPU检查太阳Lambert解析、角度与点/聚光衰减、屏幕外隐藏caster和cast开关、环境MIS期望rho×L、面积灯CPU积分、真实发光目标反射、红墙漫反弹/更深路径增能、静止计数与相机/灯失效；逐像素mean与两个固定seed的独立算术平均差<2e-6。timeline gate真实阻塞两帧红/绿灯快照，验证灯/header/descriptor隔离与历史失效。原光栅材质/阴影/IBL/生命周期全保留回归。

源码指纹：`c6a5ae66eaaeb0708eecf84df606b757fbd8f678d5d74883bfa65e6cc169d449`。原glTF/bin与固定SHA匹配。首次基础回归仅因主pass更名的断言失败，标签断言更新；未放宽数值/图像断言或原90/120秒时限。没有validation layer/原生Wayland接受声明。

生产viewer原生1920×1080、2 slots、5s warmup/10s sample：

| 相机 | GPU有效帧 | GPU total中位数 | main中位数 | slow acquire |
|---|---:|---:|---:|---:|
| static | 204 | 48.720ms | 48.577ms | 0 |
| orbit | 784 | 1.951ms | 1.904ms | 0 |

main包含TLAS build＋path tracing＋累积，不是纯遍历；两个相机路径覆盖不同区域，不能把差值作为优化收益。静止厨房约49ms，尚不满足60FPS；初始无硬RT FPS的范围保持。全部原始帧、模式/query标签匹配，pending present与prepared/retired/staging归零，未筛除慢帧。GPU预览为原生测试窗口942×1012、少量spp/EV0/no-tonemap，raw HDR与PNG均保存，不当作正式最终画质。

下一步RT-C：共享IOR/transmission/absorption、闭合实体与开口薄片诊断、折射/TIR和光栅近似同步；RT-D成熟降噪+透射独立处理。CPU对象BVH/Hi-Z/SDF仍后置。没有commit/push。

## 原目录最终交付

34个源码/文档文件逐文件hash匹配、旧版本备份。原build-linux构建成功，源码指纹与staging的c6a5ae66eaaeb0708eecf84df606b757fbd8f678d5d74883bfa65e6cc169d449一致；原目录八项烟测8/8、2.68s。原资产SHA保持，无commit/push。完整45项与两个1080p配置的数据保留，当前不是物理玻璃/成熟降噪或60FPS接受。下一步RT-C。
