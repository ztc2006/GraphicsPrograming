# RT-A：独立主光线管线决定

2026-10-09。用户“推进光追管线”确认完整实施清单，授权从RT-A执行。

采用Vulkan RT Pipeline/SBT，独立发射相机主光线，独立RGBA32F HDR，经共同曝光/显示变换输出。第一增量是主可见性和基础材质颜色，阴影/反射/GI在RT-B、实体玻璃在RT-C、成熟降噪库在RT-D，不能把基础预览标成完整路径追踪。

共享原有GPU网格和材质纹理：支持设备的vertex/index buffer增加BDA/AS build input用途；Vertex112B通过scalar布局读取。独立GPU实例表保存地址、材质颜色/alpha、贴图索引。固定512个完整填充的贴图描述符，不使用variable count、update-after-bind或部分绑定；材料超限时仅禁用该场景的RT模式并给出原因，保留光栅场景加载；切回较小场景恢复RT资格，不越界。

设备可选协商AS、RT Pipeline、deferred host operations、BDA、scalar block layout、int64与采样图像非均匀索引；无能力设备仍运行光栅。VMA仅在BDA启用后增加对应allocator flag。新shader明确Vulkan1.3/SPIR-V编译目标并进入源码fingerprint。

BLAS在候选scene的同一UploadBatch中构建，与既有上传fence一起完成后提交；BLAS scratch独立记入Staging临时域，在上传完成后释放；TLAS scratch按帧槽持久保留，失败或取消沿用候选所有权与旧场景保留。每帧槽拥有独立TLAS、实例输入、材质表和descriptor set；slot fence完成后才写/替换。第一版每个RT帧重建TLAS以验证刚体移动和场景切换，静态复用/UPDATE测量优化后续，不声称已高效refit。

图增加RT storage-image写语义，明确AS build pass -> primary trace -> display。AS/host/SBT的内部依赖由RT模块显式memory barrier表达；BLAS上传中transfer -> AS输入/RT读、AS写 -> 后续AS/RT读；TLAS build -> trace按AS access同步。scratch按设备alignment手工对齐，SBT handle/stride/base按实际设备limits对齐。状态仅在submit成功后发布。完整RT实例列表不使用相机剔除列表，primaryVisible过滤保持厨房窗户光卡修复。

基础hit读取sRGB albedo、vertex color/alpha与材质tint；MASK执行any-hit拒绝，单/双面与镜像变换有独立验证。BLEND在基础预览采用固定像素coverage近似并明确尚非物理玻璃。未实现贴图的其他光照项不伪装成共享完整BRDF。基础miss采用固定背景，环境光传输在RT-B加入；此阶段不用它评价画质。

UI仅增加Raster / Ray tracing切换；非选中路径不执行scene pass，RT关闭GTAO/TAA/jitter而保留用户光栅设置，切换使光栅历史失效。重新创建RT输出资源与交换链替换一起完成，在旧frame已drain后发布。基础RT GPU主pass统计包括AS构建和trace；后续增量再分拆测量，不能称为纯trace耗时。

验证：CPU graph写->读/undefined/read-write边与能力边界；真实RTX解析三角形颜色/命中距离/法线与遮挡、MASK/单双面/镜像/变换/隐藏几何；两帧槽、模式切换、scene commit/失败、resize和最终账本归零；厨房1080p两帧实际运行。沿用全部既有回归，记录原始输出。无原生validation layer则报告缺口。

依据：RTR4第23.11/26章的主可见性、AS与采样合同；[Vulkan AS构建规范](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBuildAccelerationStructuresKHR.html)和[trace/SBT规范](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdTraceRaysKHR.html)，2026-10-09核对。BLAS到TLAS不能假定build调用内部排序；输入与scratch不因录制完成就可释放。
