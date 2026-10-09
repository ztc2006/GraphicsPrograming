# RT-B：直接光、反射/GI与累积决定

2026-10-09，依据已确认RT-A/B/C/D方案执行，UI仍只切换Raster/Ray tracing。RTX4060Ti，正式1080p，初版RT无硬FPS；用户保留画质判断。

## 数据和管线

复用RT Pipeline/SBT及原GPU网格；raygen迭代路径，closest-hit返回位置/几何与着色法线/BRDF参数/发光信息；单独shadow miss及any-hit组处理可见性，递归深度仍为1。太阳、punctual参数与方向/单位/范围/锥角、材质因子及sRGB/UV/sampler均来自同一Scene/MaterialGpuStore。固定完整填充512纹理array按imageView+sampler去重；超预算仅禁用RT，保留光栅。

实例mask分为primary=1、secondary=2、caster=4：隐藏光卡不阻挡主相机，但可作为二次发光源；不投影者/BLEND不进入阴影mask，保留当前光栅cast语义。发光三角形从真实网格/实例世界变换构造，面积/能量CDF采样，shader读取相同发光贴图；单/双面发光独立判断。CPU保存轻量位置/索引以生成动态发光表，记录额外CPU内存，AS原存储继续复用。

## 估计器和材质边界

1 spp/帧、初始最多8次表面事件；太阳和punctual逐灯求值/真实shadow rays。面积光和环境NEE与BSDF方向采样用power-heuristic MIS；路径碰到发光体/逃到环境使用互补权重，避免双计。diffuse cosine与GGX NDF混合采样，明确单位化pdf；第三次事件之后Russian roulette并除以生存率。有限深度是明确截断偏差；不增加firefly clamp来掩盖错误。

直接光提取原光栅GGX/Fresnel/几何项为共享GLSL函数，保持既有直接光数值。路径采样用单位化GGX分布，避免把现有前向D项1e-6分母保护误当作概率密度；该保护对极窄峰的近似和前向IBL不同几何近似是既有边界，记录而不暗改光栅结果。材质模型和直接光控制共享；不宣称所有数值在光栅/积分路径间相等。

normal/MR/emissive/specular及独立UV解码接入；几何法线用于偏移和半球约束。第一个增量使用明确LOD0与法线贴图；几何导数AA、射线足迹过滤/POM与完整玻璃属于后续质量/RT-C边界，不借法线或粗糙度修改遮挡。AO不再作为RT间接光乘子；原ambient常数/probeSH/预滤镜面不叠加到真实路径贡献，环境只采全局cube原始mip0。直接/环境艺术强度在其对应源项应用，曝光仅显示变换。

## 历史、同步与精度

原独立RGBA32F目标存储运行均值，不复用光栅TAA，也不额外宣称已降噪。静止时累积；相机/几何/材质/光源/环境/设置/模式/场景/resize变化失效；display EV变化不使scene-linear采样失效。CPU sample计数和历史key只在成功submit后发布，slot-local灯/发光表/参数保持两帧隔离；新Scene失败仍保留旧资源。图增加RT storage read-write用法，保留上一shader writer的同步来源；首帧/重置明确覆写，不读undefined。GPU累积与display按单队列状态依赖执行。

原608B/128B/1088B与Vertex112B保持；RT Push96B保持，新实例144B，灯光header96B，区域光记录80B（具体ABI实现时assert）。使用几何尺度/FP32精度相关偏移而非统一翻转法线。所有pass成本记录为TLAS+trace/integrator，不称作完整降噪成本。

## 验证

解析直接光与既有共享公式、点/聚光衰减和遮挡；屏幕外/隐藏阴影者、源光卡/普通emissive单/双面、独立贴图与normal/metal/spec参数；环境/区域光MIS收敛及无双计；镜面反射真实目标命中、两次以上漫反弹。静止均值与高采样参考、运动/材质/灯/模式失效和EV保留；timeline阻塞两帧不同灯/参数/历史数据；失败/退休/resize/归零。原生RTX完整相关回归和厨房1080p raw HDR/测量，保留所有异常帧；不替用户判断画质。

## 依据

RTR4第9/11/23.11/26章用于材质/光传输/实时光追与重建；[PBRT路径积分](https://pbr-book.org/4ed/Light_Transport_I_Surface_Reflection/A_Better_Path_Tracer)与[微表面分布](https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory)用于估计器/pdf/MIS/RR；[Vulkan Trace/SBT](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdTraceRaysKHR.html)用于管线表布局，2026-10-09核对。不把离线例子耗时当作本机预算。
