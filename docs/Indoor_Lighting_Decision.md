# 室内灯光、遮挡和局部环境修正（2026-10-05）

用户确认优先修复厨房灯光/阴影和室内天空反射，再继续原路线。目标仍是
Linux/RTX 4060 Ti、原生1080p/60Hz；本轮只做 llvmpipe/X11 正确性验证。

## 选择与边界

保留已有太阳及PCF阴影，加入最多四盏重点聚光灯阴影。使用4096×2048 D32
图集：左侧2048平方太阳，右侧四个1024平方聚光灯。超过预算仍照明并明确
显示未分配阴影；点光源六面阴影、稳定CSM后续实现。每帧更新直接光和阴影。

加入一个房间范围的静态/按需局部反射探针（128平方六面）。捕获完整场景的
直接光、阴影、发光材质和可见天空；捕获时关闭几何体IBL，避免天空穿墙和
探针反馈。复用GGX预滤波、SH漫反射与BRDF LUT；房间内使用局部探针与盒式
视差校正，房间外使用全局天空。范围判断用着色点而非相机位置。此方案有
单点捕获的可见性近似，不能替代动态GI、多探针/门户或逐像素环境可见性。
移动物体/灯光后需重新捕获；UI明确显示有效/待刷新状态。

关闭全局IBL只能隐藏缺陷，不能产生室内反射；太阳阴影乘在IBL上也不能
表示全方向环境遮挡。SSR缺失屏幕外反射，光追/GI超出本轮范围。因此选择
用户已批准的局部探针作为室内最小闭环。

## 数据流、资源与同步

保留448B帧UBO和64B灯光结构；灯光cones.z保存图集索引（-1表示无阴影）。
追加608B每帧Shadow/Probe SSBO(binding9)，四个灯光矩阵/图集区域/bias、
太阳区域、计数/标志、探针范围/位置/SH；push constants扩展到128B。
图集整图深度写后转采样，PCF核限制在各自tile内，避免串扰。Graph增加
VertexRead缓冲用途，host写分别对shadow vertex/main fragment可见。

global/local预滤波共用samplerCubeArray：cube0天空、cube1房间，不增加采样器
数量；显式检查并启用imageCubeArray，检查数组层数。RGBA32F预滤波，RGBA16F
捕获和D32深度；阴影bias为深度空间近似，有UI调整。

捕获是明确的离线操作：先等待已有frame fences，六面分别记录/提交/等完成，
读回后CPU预滤波并一次上传，最后更新所有已空闲帧descriptor。捕获不参与
swapchain、不计入正常frame ID或查询，不修改正在使用的descriptor。
场景提交立即使旧探针失效。正常运行不因探针增加逐帧等待。
六面用正常右手相机和Vulkan Y翻转保持现有剔除/背面法线语义；读回时
逐行反转X，使图像屏幕right与samplerCube的s方向一致。非对称半面发光
回归必须证明反射位置正确，不能仅用纯色面验证。

## 验收

先证明已有太阳：白色无镜面接收面，无遮挡direct >0.2，遮挡后<1%。
记录旧版本的封闭室内天空反射失败。修复后闭室无灯无发光探针及金属IBL
<0.001；开窗捕获有非零天空；室内灯/发光物产生非零局部反射。
聚光灯开/关、遮挡/移动、关闭总阴影、四灯预算/第五灯降级、tile边界、
全光源/clustered、one/two slots均要覆盖。模型切换与探针刷新不得遗留资源。
完整厨房与cutaway用相同灯光预设对照，UI可切回资产灯光并重新捕获。

## 依据

RTR4第7章（shadow mapping/PCF/bias）、第9章（微表面PBR）、第11章
（环境光、反射探针、局部可见性）。官方实践核对日期2026-10-05：
[Filament](https://google.github.io/filament/main/filament.html)的SH/GGX、局部IBL与
遮挡边界；[Vulkan core features](https://docs.vulkan.org/spec/latest/chapters/features.html)
的imageCubeArray。这次调整提前完成必要的局部阴影/探针最小切片，原来的
CSM、GTAO、TAA、SSR与硬件验收仍未完成。
