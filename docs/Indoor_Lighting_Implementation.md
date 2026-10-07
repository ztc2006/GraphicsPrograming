# 厨房灯光与房间反射修复（2026-10-05）

本轮按用户确认，把 M5-B/M8-A 的最小切片提前到稳定 CSM 之前。
技术取舍、同步与近似边界见[决定](Indoor_Lighting_Decision.md)。

## 实际行为

- 原有默认太阳和太阳阴影保留。新的4096×2048 D32图集左侧仍为2048平方太阳，
  右侧最多四个1024平方聚光灯。动态灯/物体每帧更新；同一套 MASK/镜像/双面
  阴影流水线复用，PCF核限制到tile内部。点光和局部方向光不分配阴影。
- 灯光仍保留64B结构，cones.z为阴影索引，-1表示无阴影。禁用灯不占预算；
  超额灯仍参与照明。极端小/接近180°完整锥角或无有效深度区间的灯明确不分配。
- 每帧独立608B shadow/probe SSBO(binding9)在自己的fence后写入。Graph新增
  VertexRead，保留host写对shadow vertex/main fragment的可见性。448B UBO不变，
  push constants128B，fragment共4个storage buffers、16个samplers、22个资源。
- 一个房间探针：六面RGBA16F捕获，D32深度，128平方；捕获直接光/阴影/发光和
  可见天空，几何体IBL关闭。读回逐行反转X来匹配samplerCube方向；转换为
  512×256经纬图后复用SH/GGX。全局/局部存为同一RGBA32F cube array，复用BRDF LUT。
  显式检查/启用imageCubeArray；间接光强度在正常着色时只乘一次。
- 着色点位于房间盒内时选择局部SH与盒式视差校正镜面，盒外使用全局天空。
  捕获等待所有frame users；六面各提交/等待，再上传一次并更新空闲descriptor。
  捕获不 acquire/present，不增加正常frame ID或改写正常GPU查询。
- 改灯、改材质或场景变换后显示待刷新；旧探针可继续作为静态近似使用。
  模型提交使探针失效。完整透明队列供捕获/脏检查，主相机剔除不会删掉捕获内容。

## UI、启动和资产

默认`--lighting-preset auto`只识别已固定的Country Kitchen材质语义。
完整厨房/cutaway使用两盏明确的70cd暖色天花板聚光灯，加房间探针和室内视角。
源资产仍没有KHR灯光，原文件没有被改写；该预设不是作者灯光还原。

`--lighting-preset asset`保持资产灯光。Lighting提供Kitchen lighting preview、
Restore asset lighting、每盏spot的Cast shadow/投影范围/bias/PCF以及阴影可见性
检查。Room reflection probe提供盒范围、位置、启用、Capture / refresh和状态。
benchmark schema4增加lighting_preset、punctual_light_count、spot_shadow_count、
local_probe_valid，初始化捕获在计时前完成。

## 软件验收

Release/Ninja；显式llvmpipe/X11，`VK_DRIVER_FILES=lvp_icd.json`、
`MESA_VK_WSI_DEBUG=sw,noshm`，不访问真实GPU。生产指纹：
`f0512b4c1480ed60ea43a0887d95a3e4a4880843f462867c60d77062bd7ffb28`（93 inputs）。

初轮25项CTest中24通过；新Graph测试误要求srcStage只能为Host，合法屏障会
组合Host与此前的VertexRead，修正为验证必要Host写及目标阶段。增强室内测试
为真实透视相机，并断言clustered已实际启用；这三项重跑3/3通过（27.20s）。
因此25个当前检查均有通过结果：16CPU、9软件GPU。其余22项无需重复，生产图
逻辑没有因修正断言而改变。原有四事务测试保持84组full/clustered完整HDR对照。

独立读回（一/两帧、full/真实GPU clustered）：

| 场景 | HDR结果 |
|---|---:|
| 默认太阳，无遮挡/遮挡 | 0.318115 / 0 |
| 聚光灯，无遮挡/遮挡/总阴影关闭 | 1.41406 / 0 / 1.41406 |
| PCF4，靠近spot tile边缘且被遮挡 | 0 |
| 原全局IBL，墙后金属反射 | 0.882324 |
| 捕获封闭无灯无发光房间 | 0 |
| 打开+Z墙后重新捕获 | 0.868652 |
| +X半面发光墙，反射朝-X/+X | 0.0451965 / 1.77734 |

半面发光测试先得到反向结果1.77832/0.0449829，再修正读回方向；避免纯色面
掩盖镜像。还覆盖移动投影者、第五盏灯保留照明、局部红色发光反射、刷新后
去除陈旧天空、frame ID不被捕获占用、材质/几何脏状态、模型切换失效和最终
资源归零。CPU cube方向/投影/预算/盒校验通过ASan+UBSan；shadow.vert与
triangle.frag通过Vulkan1.3 SPIR-V验证。完整厨房293opaque/6blend真实导入及
主场景HDR对照另行通过（local/global差异L1=99039.8），图像与报告保存在本聊天outputs/indoor。
实际viewer启动：完整厨房auto一帧1row，asset两帧4rows，cutawayauto两帧5rows，
共10个有效GPU query rows；预设/灯数/阴影/探针和actual clustered状态匹配，
present release proven，prepared/retired/staging归零。早期1.2s asset烟测没有
测量row，已拒绝，换用warmup0/duration5s短正确性检查；这些不是性能验收。

## 保留的边界与下一项

这是单点静态捕获加盒式视差近似，不能逐像素恢复整间房的可见性，也不提供
多次漫反射反弹、实时GI或点光cube阴影。开窗/玻璃允许室外天空进入捕获；
不能把所有室内天空反射都当成错误。多探针、混合/门户、探针导入、SSR、
GTAO和TAA仍待实施。投影者目前完整绘制，保守剔除待测量。

下一项恢复 M5-A 稳定CSM。4060 Ti画质、GPU预算、原生WSI、validation和RenderDoc
验收仍按用户要求等换机通知，软件测试不能证明1080p/60FPS或完整物理正确性。
