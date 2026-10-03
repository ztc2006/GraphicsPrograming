# 项目设计审查

> 状态说明（2026-10-02 M1-A 后）：本文保留原设计审查时的代码依据。完整候选 Renderer 已由候选 GPU 资产集合替代，现状以阶段清单为准。用户确认目标主机为 RTX 4060 Ti，硬件验收等其通知换机后进行；旧 RTX 2060 记录不是当前目标。

日期：2026-10-02。项目：`/home/ztc233/Vulkan/Graphics/GraphicsPrograming`。

审查结论：保留已经验证的 Vulkan 和导入基础，重做资产生命周期、帧资源、通道编排和显示输出接口。现有问题足以支撑这些改动；通用编辑器、进一步扩展 ECS、Nanite 类几何系统和实时 GI 暂不进入主线。

本报告审查当前工作区，包含用户已有的未提交改动与本次已安装的修复；不是对某个提交的差异审查。Q1–Q16 已确认，项目路线及执行清单已更新；本报告保留审查时的代码事实和旧计划冲突。

## 已验证的基础

- Vulkan 1.3、动态渲染和 synchronization2 已接入；窗口、ImGui、相机、基本资源封装可保留。
- 直接光 GGX PBR、SH 漫反射环境光、方向光阴影与 PCF、alpha mask/blend、双面材质、CPU 包围盒剔除已经存在。
- 启动、UI、拖放、重新加载已统一；失败保留旧场景的语义正确，应继续保留。
- 四合院的蓝白色主因已经通过 RenderDoc 像素调试确认并修复：正反面配置错误导致法线向内翻转。UV 选择与节点矩阵导入也已修复，不能再把这些问题列为待办。
- 原项目构建及五项 CTest 已通过。这些测试主要覆盖 CPU 语义，不能据此认定完整 PBR、运动画质或 GPU 性能已经验收。

已有证据见 [RenderDoc 检查报告](/home/ztc233/Documents/Codex/2026-10-02/home-ztc233-vulkan-graphics-graphicsprograming/outputs/RenderDoc检查报告.md) 与 [验证结果](/home/ztc233/Documents/Codex/2026-10-02/home-ztc233-vulkan-graphics-graphicsprograming/outputs/验证结果.txt)。捕获使用 llvmpipe；硬件资料显示 RTX 2060，但当前没有可靠的硬件 GPU 帧时或显存容量测量。

## 优先处理的设计问题

### P1：主通道没有线性 HDR 中间结果

[主通道](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/renderer.cpp:1836) 直接写 swapchain；[材质着色器](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/shaders/triangle.frag:401) 与 [天空着色器](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/shaders/environment.frag:49) 分别执行 tone mapping。透明流水线的 blending 因而发生在已经映射的颜色上。

这会破坏 HDR 合成的一致性，并使曝光、Bloom、反射无法共享可信的场景辐射量。环境强度也不能替代全场景相机曝光。改为线性 HDR 场景颜色，再统一曝光、后处理和输出变换；透明材质在线性 HDR 中混合。显示范围先保持现有 SDR/sRGB，HDR 中间缓冲不等于 HDR 显示器输出。[Filament 的成像流程](https://google.github.io/filament/main/filament.html#imagingpipeline) 可作为校准参考。

### P1：镜面 IBL 还是占位近似

[当前实现](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/shaders/triangle.frag:329) 将原始 HDR 反射采样与低频 SH 结果按粗糙度混合。代码明确标为临时近似，不能提供正确的粗糙镜面卷积。

保留 SH 漫反射；增加 GGX 环境预过滤与 split-sum BRDF LUT，并用材质球粗糙度阶梯校验。环境烘焙结果可缓存，加载模型不应触发环境重算。[Filament IBL 推导](https://google.github.io/filament/main/filament.html#lighting/imagebasedlights) 支持此算法基线。

### P1：纹理没有 mip，sampler 语义被忽略

[纹理创建](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/texture.cpp:107) 固定一个 mip；[sampler](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/texture.cpp:160) 固定线性过滤和 Repeat。glTF sampler 没有进入资源模型。

远处屋瓦、植被和法线高频细节的采样基础不足。TAA 不能替代 mip 和正确过滤。需要区分 sRGB 颜色、线性数据与法线，生成或加载匹配语义的 mip；按资产 sampler 选择过滤、寻址与 LOD，设备支持时采用受限各向异性过滤。Alpha mask 的覆盖率与法线引起的镜面闪烁也要单独验证。

### P1：换模型重建整个 Renderer

[loadScene](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/application.cpp:1203) 建立候选 Renderer，再与旧 Renderer 交换。失败保留旧场景的事务语义正确，但事务对象范围过大：环境、阴影、默认纹理、流水线和帧资源都跟随场景重新创建。

应保留持久 Renderer，仅构建候选 GPU 资产集合。上传成功后提交新场景，旧集合在 GPU 使用完成后回收。记录旧、新场景并存的峰值显存；在完整保留旧场景的语义下，新资产超出预算就应明确失败，不能先销毁旧场景再假装可回滚。

### P1：导入器静默丢失资产语义

[accessor 读取](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/gltf_loader.cpp:641) 覆盖有限；[纹理索引](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/gltf_loader.cpp:804) 未表达 sampler；[材质解析](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/gltf_loader.cpp:880) 未验证必要扩展。

四合院使用的 `KHR_materials_specular` 尚未解析。缺失 sparse、normalized accessor、原始 tangent 等语义时，不能仅凭“模型显示出来”判定兼容。成熟解析库负责格式读取，项目适配层负责颜色空间、UV、材质扩展和错误策略，并保留现有导入回归测试。未知 required 扩展拒绝导入；可选未支持扩展明确警告。

OBJ 当前会居中和缩放，glTF 保留作者坐标；两者应统一到明确的场景单位策略。建议保留原始几何坐标，默认通过相机适配观察，OBJ 的规范化作为显式选项。

### P1：资源依赖和帧生命周期不足以承接新效果

[Application 绘制循环](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/application.cpp:381) 决定阴影与主通道顺序，[Renderer](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/renderer.cpp:1785) 手工管理通道状态。主深度只有 attachment 用途，而且 [storeOp 为 DontCare](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/renderer.cpp:1832)。没有场景 HDR、可采样深度、法线/粗糙度、运动矢量或历史结果。

这些是 AO、SSR、TAA 的真实依赖。采用范围受控的单图形队列 Render Graph，显式声明读写、资源状态、清除/保留和生命周期，逐通道迁移。不要把资源别名、多队列和异步计算一并塞进第一版。

## 应尽早测量并改进的结构

| 项目 | 代码证据 | 改进与证据边界 |
|---|---|---|
| 上传同步过重 | [texture.cpp:233](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/texture.cpp:233)、[texture.cpp:287](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/texture.cpp:287)、[device.cpp:88](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/device.cpp:88) | 多次单独提交并等待队列，改为批量 staging 上传和完成信号；目前没有加载耗时数据，不声称已测得瓶颈。 |
| 纹理所有权附着在材质槽 | [material_gpu_store.hpp:20](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/material_gpu_store.hpp:20)、[material_gpu_store.cpp:104](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/material_gpu_store.cpp:104) | Image、Texture/View、Sampler、Material 分开标识和缓存。颜色空间/用途不同不能盲目按路径合并。先消除重复上传，再评估 bindless。 |
| 只有一帧在飞行 | [renderer.hpp:116](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/renderer.hpp:116)、[renderer.cpp:624](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/renderer.cpp:624) | 可测量两帧方案；需要分配帧级深度/阴影/临时资源并处理历史访问和退休资源，不能只改常量。比较吞吐、显存和输入延迟。 |
| resize 重建范围过大 | [renderer.cpp:96](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/renderer.cpp:96) | 按尺寸与 attachment 格式依赖重建，保留固定阴影和无关流水线。当前没有 resize 延迟测量。 |
| 每次 draw 重绑状态；阴影无光源视锥剔除 | [renderer.cpp:697](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/renderer.cpp:697)、[application.cpp:421](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/application.cpp:421) | 先做稳定状态排序、阴影剔除和脏标记；深度预通道与复杂剔除用开关对比成本。 |
| 只有 CPU 整帧统计 | [application.cpp:352](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/application.cpp:352)、[application.cpp:790](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/src/application.cpp:790) | 增加 GPU timestamp、pass 标签、上传耗时、显存峰值与 p95 帧时。CPU 整帧耗时不能替代 GPU 时间。[Vulkan profiling 指南](https://docs.vulkan.org/guide/latest/profiling.html)。 |

源资产统计：四合院约 59.7 万三角形、27 个材质 primitive；57 个贴图槽引用 45 张图像。当前每槽独立上传，存在共享资源去重空间。这些统计不代表本机稳态 GPU 瓶颈；“场景大”也不等于“draw call 多”。Sponza_2 有 381 个 group，可补充提交压力测试。

## 需要判别测试的疑点

- 负行列式变换会改变绕序，而当前流水线选择没有区分镜像变换。全局 CCW 修复已经完成，但仍应单独测试负缩放的单面物体与阴影；在重现前不把它判为现有画面错误的确定原因。
- 导入后主要保存展平的世界变换。静态显示可用；若移动父节点需要子节点跟随，则需保留节点身份、父子关系与局部变换。第一轮只补满足场景变换所需的数据，不扩大成通用编辑器 ECS。
- 当前 vector 索引式资产 ID 足够支持整体场景替换；增量卸载/复用时才需要代际句柄。不要先增加复杂度再寻找用途。

## 旧计划为什么必须改

| 审查时的旧版文档 | 与现状的冲突 | 本次处理 |
|---|---|---|
| [Engine_Roadmap.md](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/docs/archive/Engine_Roadmap_before_2026-10-02.md:3) | 更新停在 2026-05-25；仍写 Scene 拥有 mesh/object、禁止当前 ECS/Render Graph、PBR/IBL/阴影不在范围内；“下一步提取材质 GPU 资源和第一张阴影图”已经完成。 | 已整体重写，采用新产品边界、依赖和验收线。 |
| [Renderer_Refactor_Checklist.md](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/docs/archive/Renderer_Refactor_Checklist_before_2026-10-02.md:102) | 一处要求先销毁旧资源，另一处要求失败保留旧场景；还以 Windows run.ps1、三角形为验收。 | 完整旧版归档；当前文件替换成新的阶段执行清单。 |
| [OrbitCamera_ImGui_规划.md](/home/ztc233/Vulkan/Graphics/GraphicsPrograming/docs/archive/OrbitCamera_ImGui_before_2026-10-02.md:10) | 计划 orbit、不支持 WASD，而实际已是自由相机。 | 完整旧版归档，原入口标为历史；相机仅保留可复现路径和检查画面的任务。 |
| [进度记录](/home/ztc233/.codex/skills/vulkan-engine-progress/references/progress-history-2026-10-02.md:1) | 当前快照与历史决定仍混杂“不引入 Graph”、Blinn-Phong、尚未提取材质等过期描述。 | 已更新当前快照；旧记录完整归档，旧决定标为已替代。 |

## 技术选择结论

以 Clustered Forward 为多光源方向，先建立普通多灯 forward 对照；保留已有透明路径，减少主材质通道整体迁移。Deferred 在不透明过绘制或复杂材质需求证明收益后重新评估。任何 light-list 溢出都必须可见且有正确性回退。

采用稳定 CSM 与有限聚光灯阴影；TAA 承担运动稳定性目标，mip 和镜面抗锯齿提供采样基础。AO 采用 GTAO 类方案；局部反射采用探针与 SSR 的覆盖互补。动态 GI、光线追踪和重建式上采样后置。

这组选择是结合本项目约束作出的工程判断，不是声称这些算法在所有项目都最优。完整交付、替代方案、阶段依赖与测量方法见 [现代渲染器路线](./Engine_Roadmap.md)。
