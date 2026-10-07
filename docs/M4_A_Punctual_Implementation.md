# M4-A：多光源普通 forward 实施与验收

2026-10-05。实现依据见 [代码前决定](M4_A_Punctual_Decision.md)，参考 RTR4 第5/9/20章以及当前 Khronos punctual 规范和 Sample Renderer。M3-C2 本地改动保留。本阶段完成点光/聚光/导入方向光的正确性基线；clustered 和新增灯光阴影留给 M4-B/M5。

## 场景与编辑

`KHR_lights_punctual` 支持 required/optional，glTF/GLB 路径一致。三种类型、缺省、线性颜色、cd/lux、米/range、内外锥角通过共享校验。所有灯定义都校验；只实例化所选场景节点。灯在导入时按层级世界矩阵烘焙位置与 -Z 发射方向，尺度不改变强度/range/锥形。多个节点引用一个定义得到独立实例。

候选灯在后台准备前按实际设备上限校验；候选的 CPU 灯与 GPU/网格/材质/camera 一起发布。失败/取消保留已编辑 live lights；切换无灯资产清空旧灯。含灯资产自动关闭默认太阳以避免重复照明，无灯资产恢复默认太阳；仍可用 Default Sun 手动开关。环境照明保持独立。UI 可添加/移除/启用点光或聚光、编辑类型、世界位置、发射方向、线性色彩、强度/range/锥角，显示新增灯暂不投影；Direct Lighting 提供经过统一 EV/display 的辐射量视图。实例采用世界空间快照；导入后移动某个 mesh 不隐式移动另一个 light，运行时父子动画绑定后置。

cgltf 1.15 的最小本地补丁增加 range/spot 存在性，以区别省略无限距离与显式0非法，以及缺失必需 spot 对象。原 pin、许可证、原始 SHA256 保留，本地 ABI/hash 另记于 [补丁](../deps/cgltf/LOCAL_PATCHES.md) 和 provenance，所有编译目标一致使用该头。

## 渲染与帧所有权

普通 forward 遍历全部活动灯。每灯独立 L/H/NoL/D/G/F，共用 C1 specular 因子、混合 F0/F90/介电 scalar diffuse 与 C2 过滤粗糙度。平方反比、range 四次窗和平方 cosine ramp 跟随 Khronos 推荐；方向光无距离衰减。IBL/emissive 合成一次，AO 只调制间接光。现有全局 diffuse/specular 调节仍保留，单位增益取1时与数值BRDF参考一致；没有绝对显示亮度标定的承诺。旧太阳可用旧阴影；新增灯均无阴影。

每槽拥有 set0 binding6 只读 SSBO：16B header + 64B记录，起始64容量即4112B。零灯写0；第65灯增长至128，保留全部。maxStorageBufferRange 与 uint32 限制给出整体报错，不截断。CPU 校验/打包在 acquire 之前，等待当前 slot fence 后才写/扩容/改该槽描述符，其他 pending 槽保持原有存储。VMA write flush 与 submit 可见性负责 host→device，同步不增加图 pass 或上传 wait。旧 shader/pass 和 scene retirement 所有权沿用。

Frame UBO448、Material UBO48、Vertex112、push112、16 combined samplers 保持；storage descriptor 每槽新增1，fragment resource limit 检查19，实际 storage stage/set/range limits 均检查。依然默认一帧、可选两帧。FP32 运算进入 RGBA16F；中心距离<0.1mm贡献0，极窄锥 cosine 相等时硬边近似。任意极高强度/贴近亮光滑金属的 FP16 溢出尚无无限范围承诺，pre-exposure/FP32 压力方案后续按真实证据选择。

## 软件验收

21/21 CTest：14 CPU + 7 llvmpipe/X11 sw,noshm GPU，0 skipped，完整运行 197.32s。四种 transaction 配置均验证新增功能和旧 C1/C2/IBL；没有把软件耗时用作4060Ti预算。

- CPU：独立衰减常数、range/锥边界、中心近似、64→65增长/actual limit、禁用灯、坏输入整体拒绝；glTF/GLB 默认、实例/selected scene、层级尺度/旋转、颜色/单位保留及非法range/intensity/color/spot/direction。
- 生产 HDR 对独立 double BRDF：0/1/16/32/64/65灯，d=1/2、range边界、spot内/中/外和极窄锥、中心、方向/点光移动、相机/物体移动、dielectric/metal/specular/C2 normal AA、MASK/BLEND、AO不暗直接光/emissive仅加一次、错误帧拒绝后可重试。逐 RGB 容差 max(.003,.007×expected)。
- 两槽由 unsignaled timeline gate 同时阻塞：65/1灯不同快照，一槽64→128扩容期间另一槽保持64容量，生产HDR分别 .25/.75；query只交付一次、ID不回退、scene/UI按最后完成帧退休，最终账本归零。
- 真实 ImGui异步加载：灯随候选发布，失败/取消保留123cd编辑值，切换无灯资产清空；reload/cache/cancel/upload/preview/resize/shutdown仍通过。原事务仍4 commits/6 frames/5 uploads/7 image copies/0 runtime upload waits，environment/pipelines仍1/12。
- CPU light/import/cgltf/Mikk ASan+UBSan通过；SPIR-V validation 与 offsets0/16/32/48、stride64、header16、set0 binding6对应。

参考场景 [punctual_reference.gltf](../assets/render_tests/punctual_reference.gltf) 包含32点光+spot+directional及原材质对照；一/两帧加载/绘制/报告/退出通过。报告烟测1/auto、2/fence、2/legacy均通过。四合院真实 UI 原生1920×1080一/两帧各 3/2 行，query/present逐行对应、unique IDs、pending present=0、扩展释放证明与准备/退休/staging归零一致。

| 四合院软件报告 | 一帧 | 两帧 |
|---|---:|---:|
| engine-owned buffers | 85 | 87 |
| payload bytes | 374158412 | 374162972 |
| suballocated bytes | 374516916 | 374521476 |
| unique backing bytes | 400047744 | 400047744 |

相对C2实测：每槽+1buffer/+4112 payload与suballocation，两帧比一帧+4560B，image/view/sampler和backing数量/bytes保持，详见验证JSON。账本是引擎-owned资源，不能叫进程总VRAM。源码指纹 `4361c74cb88ad1dc49c883115ee6bf0d68984f733a620cc90406fc596ca3df85`（82输入）与生成build header及两份报告一致。

本轮本地实现，尚未commit/push；证据与独立M4-A patch放在当前Codex任务outputs，progress保留最新入口。硬件GPU预算/运动画质/VRAM/native WSI/validation/RenderDoc仍等用户换回4060Ti；既有legacy释放证明和原生MIT-SHM/DRI3问题继续开放。下一项M4-B：先确定cluster空间/深度切片、透明覆盖、无限range、溢出回退、同步及测量合同，再以本全灯路径对照实现light culling。
