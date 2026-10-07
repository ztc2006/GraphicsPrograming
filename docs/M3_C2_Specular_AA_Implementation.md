# M3-C2：法线方差与镜面抗锯齿已接入

2026-10-05。图形代码前使用实时渲染技能、RTR4 第9章及原论文/官方 Filament 源码，先写 [决定](M3_C2_Specular_AA_Decision.md)。本增量本地实现，尚未提交或推送；最近 GitHub checkpoint 为 b37837d（M2-B 至 M3-C1）。

法线 RGBA8 的 A 现为均值长度损失，LOD0 A=0、RGB 原字节保留，mip RGB 仍单位方向。下级从未归一化 float3 一阶矩传播，double 面积加权，不从上一级量化方向重算。缓存策略版本 3；仅 Normal policy 使用此语义，albedo/opacity/coverage/specular 的 A 不变，sampler 和独立 UV 继续保留。完全抵消返回 +Z/损失1，平面损失0；原法线 alpha 不参与材质透明度。

材质按 d/(1-d) 的有界 Toksvig 风格估计和 normalScale² 小斜率近似加粗，几何法线在贴图扰动前取屏幕导数（0.15 方差/0.2 核上限）。在 alpha discard/POM 分歧前求几何导数和 normal UV 梯度，normal 使用 textureGrad，保持 POM 偏移。结果 r'=(min(1,r⁴+kt+kg))^¼，同时用于直接 D/G 与环境 cube LOD/LUT、原粗糙度 Schlick；GGX、LUT 和环境 cache 算法版本未改。

Lighting 新增默认开启的 Specular AA，复用原未使用 shininess 的 frame lightingParams.z；每帧槽写开关，材质描述符不在途修改。Roughness 为过滤值，新增 Authored Roughness、Normal Mip Kernel、Geometry AA Kernel。数据视图 9–13（包括先前 Weight/F0）现和原数据视图一样绕过曝光/色调映射；IBL 仍走正常显示输出。

参考场景 `assets/render_tests/normal_aa_reference.gltf` 可在 Scene UI 加载，或项目根目录 `./run.sh assets/render_tests/normal_aa_reference.gltf`。两行介电/金属球，五列粗糙度 .04/.08/.2/.4/.8，128² 高频法线；前景包含低频、scale0、镜像平面控制，法线原 A=17 用于确认忽略作者 A。地面中性灰。用 AA 开关及四个粗糙度/核 debug 对比；这项软件加载烟测没有验收最终运动画质。

Release/Ninja 最终 **20/20 CTest（13 CPU + 7 llvmpipe/X11 GPU，无跳过）**；法线 mip ASan/UBSan 和生产 vert/frag SPIR-V 验证通过。CPU 独立源空间一阶矩参考覆盖非单位输入、多级不等子分布、POT/NPOT/1D、量化、倾斜平面、相反法线、LOD0 RGB/A、版本隔离，普通 alpha/coverage 回归保持。

四种 GPU 事务验证实际 mip 完整回读/三线性 A、不同频率随 LOD 的损失与同图不同 sampler 共享；生产 shader 验证 authored/filtered/floor/saturation、平面、scale0/正负/放大、镜像/非均匀变换、MASK/POM、AA 开关、独立光栅 quad 几何差分、HDR 直接光、带独立仿射标记的 cube/LUT 粗糙度轴、金属/介电/掠射、reload 无复制/坏 normal 替换回滚/最终归零。HDR 使用 max(.012,.005×expected) 容差，粗糙度默认 .002，几何独立差分粗糙度 .0003。EV=3 的数据视图必须恢复 exposure0/toneMap=false，IBL 保持 EV3/正常显示。

M3-C1 因子/颜色/UV/金属/opacity/直接光/IBL 和旧 alpha/POM/Mikk 合同仍通过；一/两帧、async UI 加载/取消/失败/退休及最终账本归零通过。原事务仍 **4 commits / 6 frames / 5 scene submissions / 7 scene image copies / 0 runtime upload fence waits**，环境/管线 **1/12**，冷环境 **3 copies/1 submit/1 wait**。

一帧 auto / 两帧 fence / 两帧 legacy 报告 smoke 行数为 [1, 1, 1]，参考场景一/两帧为 [1, 1]。四合院 1080p/UI/orbit/FIFO/fence 一/两帧分别 3/3 完整 query/present 行，pending0、扩展资源释放证明成立，prepared/retired/staging0；单次软件加载 3.95/3.99s，不作性能归因。

相对 M3-C1 同设置，当前 GPU 账本所有对象/payload/suballocation/backing **零增量**：57 images/77 views/8 samplers/23 backing blocks，backing400,047,744B；payload 一帧374,154,300B、两帧374,154,748B，差448B。Vertex112B、Material48B、Frame448B、push112B 与 combined samplers16 不增。CPU 工作层另需相邻两层 float3 临时存储，常见方形峰值3.75B/源 texel（1024²为3,932,160B），这不属于 GPU 账本，也不是总 VRAM。峰值实测在聊天 verification JSON 保留。

生产源码 SHA256 `3a8e3c3a6ef4913c286c5514b95f554249cf62f560ec95f6dc104038109c4d8e`（80输入）与构建头/两份四合院报告一致；聊天 outputs/m3c2-verification.json、test/sanitizer/SPIR-V、报告/参考烟测、增量 patch/进度快照保留证据。

边界：各向同性近似无法精确卷积 GGX；normalScale² 只在小斜率下近似，8-bit损失量化会漏极小方差，mip 内双线性额外方差/LOD0闪烁没有覆盖。非 mip sampler 保留作者语义；height-only 贴图未烘焙方差，POM 足迹仍以原 normal UV 梯度近似。镜面过滤不是 TAA。4060 Ti 原生 WSI、motion/最终画质、GPU 时间/VRAM、validation/新 RenderDoc 继续按用户要求延后，没有物理 GPU 探测。

下一项 **M4-A：普通 forward 点光/聚光灯正确性基线**。先明确单位/衰减/范围/spot cone、导入/UI/每帧不可变 light snapshot、GPU packing/overflow、透明和材质一致性，测试1/16/32/64灯，再推进 clustered light culling；CSM 保持 M5，不把局部阴影提前混入第一项。
