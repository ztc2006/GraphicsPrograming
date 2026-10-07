# M4-A：多光源普通 forward 基线决定

2026-10-05，代码前使用 real-time-rendering-advisor。M3-C2 软件证据已完成；本轮保留此前未提交改动。Linux/Vulkan1.3、RTX4060Ti、原生1080p/60FPS、一太阳+数十局部灯、静态场景与动态变换约束已确认；硬件验收继续延后。

选择逐像素遍历所有活动 punctual lights 的普通 forward，作为后续 clustered 剔除的正确性参考。opaque/mask/blend 共用材质 BRDF；原太阳继续使用原阴影，额外 direction/point/spot 第一版不带阴影。先让单位、空间、能量、上传与编辑可信，不同时引入 light culling/CSM。

| 候选 | 本轮取舍 |
|---|---|
| 固定64灯 UBO | 简单但溢出丢灯风险、后续容量和剔除接口不便 |
| 每帧 SSBO | 增一个 storage descriptor、无需 sampler；运行时数组/容量增长保留所有灯，采用 |
| CPU剔除/clustered/tiled | 需要额外可见性/溢出/深度证据；先用全灯循环作为参考 |
| deferred/multipass累加 | 增加带宽和透明/材质通道，现有 forward 及M3合同可复用；后置 |

数据流：导入资产的 punctual light 实例（节点世界位置与 normalize(world*(-Z,0)) 方向；range/intensity/cone 不缩放）→候选 CPU lights 先按实际设备 storage limit 校验，再随 scene commit 原子替换→UI 编辑世界位置/方向/线性色彩、强度、range/cone/enabled→每帧 LightingSettings 快照→校验/打包→等待当前 frame slot fence→写当前槽 SSBO→绘制。失败/取消的候选不替换 live lights，OBJ无额外灯时清空上个 glTF 的 lights；含灯资产默认关闭原太阳（UI仍可打开）；无灯资产恢复原太阳，环境仍独立。

GPU std430：16B uvec4 count header + N个64B记录（position/range、direction/type、linearColor/intensity、innerCos/outerCos/保留），初始64容量即4112B/槽，0灯也写count0。默认1/可选2槽独立存储；超过64增长容量并保留全部，最大由 maxStorageBufferRange/uint32 确定。校验/打包和成长失败在 acquire/reset-fence 之前报告错误，不静默截断、不改在途槽描述符。替换仅在该槽 fence 完成后，其他槽继续独立；旧buffer可以安全释放。host writes使用VMA flush，queue submit提供host→device域可见性，不新增图pass/上传wait。set0 binding6，combined samplers仍16；检查 storage limits、maxPerStageResources≥19。报告GPU payload预计每槽+4112B/一个buffer，一个/两个槽总差448+4112B；具体range/backing实测，不叫总VRAM。

单位：glTF点/spot强度cd，directional为lux，RGB线性乘子，距离按glTF米；现有太阳数值视为同一直接光的照度标度，默认画面保持。numeric photometric RGB沿用Khronos sample的场景线性标度与统一EV，没有宣称绝对显示亮度/光谱标定。点光为I/d²；range>0时乘clamp(1-(d/range)^4,0,1)，边界外为0；0表示运行时无限范围（导入时须区分作者显式0非法与缺省）。距离<0.0001m返回0方向/贡献，其他距离平方下限1e-8，避免数学奇点，这是明确的点发射器中心近似。spot在内外cos之间线性插值后平方，inner<outer≤π/2；极窄锥量化相等时采用硬边近似，中心仍为1。

每灯独立H/Fresnel/D/G及NoL，使用C1混合F0/F90/介电scalar diffuse和C2共同过滤粗糙度。IBL与emissive只合成一次；AO只作用间接光，原太阳shadow不遮蔽无阴影局部灯。FP32 shader计算，RGBA16F目标不扩展；极高合法强度/近灯/光滑金属可能超过FP16动态范围，实机HDR压力验收要据证据评估pre-exposure/FP32，不能凭短软件样例声称无限范围保证。

格式校验：cgltf目前不保留range/spot存在性，给该头做有记录的最小has_range/has_spot适配补丁，保留上游pin和原始hash/provenance。拒绝非有限/负intensity、颜色超[0,1]、显式range≤0、spot缺失/非法角、退化direction；支持required/optional extension，验证重复实例/selected scene/层级尺度/旋转/矩阵。不要再写一套JSON解析器。

验收：CPU独立衰减/角度/packing/64→65成长/限制/坏输入；glTF默认、required、三类灯、未选中节点、重复实例、变换、错误回滚。生产HDR与独立double参考比较0/1/16/32/64/65灯、d=1/2、range边界、spot中心/过渡/外部、中心奇点、directional、HDR与dielectric/metal/specular/normalAA、透明/MASK，灯/相机/物体移动；一/两帧pending SSBO/UBO隔离、reload/cancel/退休/归零和旧20项测试。容差max(.003,.007*expected)，HDR bright场景可重复；SPIR-V offsets0/16/32/48与array stride64验证。单独参考glTF和真实四合院UI/1920x1080报告source/ledger一致；软件帧率不代替hardware预算。

证据：RTR4第5章点光/聚光（本地PDF111–114页，零距离和平方反比限制）、第9章BRDF/光度量及第20章多光源路径。[KHR_lights_punctual规范](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_lights_punctual/README.md) 与 [Khronos官方Sample Renderer punctual.glsl](https://github.com/KhronosGroup/glTF-Sample-Renderer/blob/main/source/Renderer/shaders/punctual.glsl) 于2026-10-05核对。没有新增post-RTR4算法，此轮用稳定模型和当前官方生产参考；下一M4-B再按测量比较clustered/tiled。4060Ti原生WSI/GPU时间/VRAM/最终画质/validation/RenderDoc仍待用户换机通知。
