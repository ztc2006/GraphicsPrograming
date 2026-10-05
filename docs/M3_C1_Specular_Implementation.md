# M3-C1：KHR_materials_specular 已接入

2026-10-04。按 [实现前决策](M3_C1_Specular_Decision.md) 和实时渲染技能完成导入→独立 UV→不可变 GPU 材质→直接光/IBL 的语义链路。四合院 Leaves.001/Bark.001 的嵌入颜色贴图与大于 1 的线性颜色因子现在进入着色器；此项结束原先的 optional core fallback。旧记录“CPU 保留 specular”只适用于 cgltf 内部解析，旧项目 Material 曾丢弃它，已经更正。

## 材质和资源合同

强度取线性贴图 A×factor；颜色取 sRGB 解码 RGB×linear factor。factor 缺省 1；颜色允许大于 1。导入拒绝非有限/负颜色、强度不在 [0,1]、缺失 UV/图像与 unlit/specGloss 同材质冲突。其它未知 required 扩展仍拒绝，optional 警告仍随场景发布。没有引入 IOR、透射、clearcoat 等额外支持承诺。

介电 F0=min(.04×color,1)×strength，F90=strength；纯金属仍使用 baseColor、F90=1。直接光按介电/金属响应混合，介电漫反射用 1-max(Fresnel RGB)；避免有色高光产生反色。镜面 IBL 用混合后的 F0×A+F90×B，沿用 M3-B 的单次散射 LUT/预过滤。SH 漫反射保留粗糙度 Fresnel 近似，先计算未乘强度的介电响应再乘强度、取 scalar max 和 (1-metallic)。既有单次散射能量损失与 split-sum/SH 近似边界继续存在。

新增材质 binding 10/11，分别为线性强度和 sRGB 颜色；缺失纹理复用已有白色 fallback。每材质 11 个 combined samplers，加帧布局 5 个，共 16；布局创建前检查实际设备限额。Material UBO 48 B，三个 vec4 的偏移为 0/16/32；Vertex 112 B，追加 UV 偏移 96/104、属性位置 10/11，alpha 位置 9/偏移 92 保持不变。Frame UBO 448 B 与 push 112 B 保持。后续再增加材质贴图时必须重新评估采样器布局。

两张纹理独立 UV、KHR_texture_transform 和 sampler；缓存继续按精确编码内容、颜色解释与 typed mip 身份共享。glTF specular 不参与 opacity/coverage。场景 UBO/描述符在候选准备时创建，发布后不原地修改；因子变更通过资产重载进入新集合。沿用批量上传、主线程提交/就绪、最后完成帧退役和失败保留旧场景。

## 使用与参考

空场景启动后，在 Scene 面板载入 `assets/render_tests/specular_reference.gltf`（也可 `./run.sh assets/render_tests/specular_reference.gltf`）。参考沿用基线几何：上行介电、下行金属，五列为核心中性、零强度、.35 有色、.6 高颜色因子/F0 钳制、黑色 F0；粗糙度 .35。其余 alpha/镜像参考保留。黑 F0 与零强度在掠射角应不同；金属参数不被扩展改变。四合院仍走原 Scene/Load/Reload 链路。

Materials 显示强度/颜色因子及两张图的 UV 和来源；Lighting/PBR Debug 新增 Specular Weight 与 Dielectric F0。保留 Base Color、Normal、Roughness、Diffuse/Specular IBL 和阴影 debug，用固定曝光/环境强度检查各项。

## 验证和成本

Release/Ninja 最终 **20/20 CTest：13 CPU + 7 llvmpipe/X11 软件 Vulkan，无跳过**。CPU 覆盖 required/缺省、真实四合院、独立坐标/变换/采样器及坏输入，导入器、cgltf 和 Mikk 的 ASan/UBSan 通过。生产 vert/frag 经 spirv-val 验证，反射偏移 0/16/32 和 binding 10/11 与 CPU 合同一致。

四种 GPU 事务分别验证：strength=0/.35/.6/1；neutral/colored/black/F0 saturated；metallic=0/.4/1；NoV=.1/.5/1；直接光、镜面/漫反射 IBL 与独立 double CPU 数值参考。颜色经 sRGB 还原，强度读 A 而非 R；重复与夹边采样器、不同 UV、opacity 独立、debug、HDR/EV/环境强度、同内容不同解释、reload 零新增图像复制、坏替换保留旧集合与最终归零通过。HDR 容差 max(.008,.003×expected)。原始事务继续 4 commits/6 frames/5 scene submissions/7 scene image copies/0 runtime upload fence waits；环境/管线仍 1/12，环境冷上传 3 image copies/1 submit/1 cold wait。

报告 smoke 一帧/auto、两帧/fence、两帧/legacy = 1/11/10 行。参考场景一/两帧各 1 完整帧；这是加载、绘制、schema 和 drain 检查，不是吞吐测量。四合院 1920×1080、UI/orbit/FIFO/fence，一/两帧分别 3/4 完整帧/query/present，pending=0、扩展释放证明成立，staging/prepared/retired 归零；加载单次观察 3.70/3.63 s，不作性能归因。

四合院当前各 **57 images / 77 views / 8 samplers / 23 backing blocks**；backing 400,047,744 B，峰值 716,188,664 B。payload 一帧 374,154,300 B、两帧 374,154,748 B，差仍 448 B。相对 M3-B payload +23,528,616 B：771,453 个场景顶点新增两组 float32 UV（+12,343,248 B），27 个材质 UBO（+432 B），8 个持久 debug 顶点（+128 B），两张 1024² 完整 mip specular color 图（+11,184,808 B）；sampler 共用未增加。suballocation +23,530,496 B、backing +22,827,264 B。这里记录的带宽/存储成本必须在后续优化时考虑，未作硬件耗时结论。账本不含 CPU、driver、swapchain 或 ImGui 额外资源，不是总 VRAM。

生产 source SHA256 `bfd4a645821b0c1c327c74ee832a904608991402ba78f41660c3a041c1c707e6` 与 build header/两份四合院报告一致。聊天 `outputs/m3c1-verification.json`、test/sanitizer/SPIR-V、报告、进度快照和 `m3c1-specular-only.patch` 留存证据；补丁从本轮开始时状态生成并验证可应用/内容一致，保留前序本地改动。本增量与 M2-B 至 M3-C1 纳入同一次集成提交。

软件验证显式使用 `VK_DRIVER_FILES=lvp_icd.json` 与 `MESA_VK_WSI_DEBUG=sw,noshm`，生产启动未强制此环境。4060 Ti 原生 WSI、GPU 时间/VRAM、最终四合院画质、运动高光与新 RenderDoc/validation 等用户换机通知；没有宣称蓝白/错位全部解决。下一项 M3-C2 法线方差过滤/镜面抗锯齿，仍先参考实时渲染技能。
