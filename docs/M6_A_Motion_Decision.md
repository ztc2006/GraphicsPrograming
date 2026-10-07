# M6-A：运动矢量与采样抖动的实施决定

日期：2026-10-06。约束沿用已确认路线：Linux/Vulkan，4060 Ti，原生1080p60，静态网格但相机/实例可动，双帧资源；硬件验收暂缓。本增量交付可读回的运动数据及 jitter 合同，TAA 历史颜色/resolve 属于 M6-B。

选择主场景 MRT 输出 RGBA16F 运动数据。复用主材质的 alpha discard、深度、镜像与双面剔除，避免独立速度 pass 在 MASK/POM 上与颜色覆盖不一致。额外几何 pass 更容易隔离，但重复提交和片元处理，而且材质语义容易分叉；仅由深度重建相机运动更省输入，却缺失物体运动。这里保留现有13条管线，仅增加相同格式的第二颜色输出和顶点阶段输入。1080p 运动目标有效像素载荷16,588,800B，实际VMA分配/驱动驻留另测。共享目标通过单队列 Graph 串行依赖，CPU输入每槽独立。

## 数据和坐标合同

- 稳定身份是 `DrawItem.objectIndex`，由 SceneEcs 的场景对象索引提供，场景提交创建新的历史代。未提供身份的兼容绘制明确无物体历史；同帧重复的显式身份拒绝。相同网格的不同实例不得混淆；mesh/material 更换失效。
- CPU先准备完整对象集合及相机快照，只在成功 queue submit 后发布为下一帧的 previous。acquire OUT_OF_DATE、无效参数及探针捕获不推进历史或采样序列。槽0/1独立保存上传内容，上一帧由提交顺序确定。
- 速度为未抖动 `currentUV - previousUV`；Vulkan正高度viewport，UV向右/向下，NDC转换 `uv=ndc.xy*.5+.5`。输出xy速度，z历史有效度，w前帧投影深度[0,1]。M6-A的w是FP16诊断值，M6-B正式history-depth rejection前必须替换为前帧线性深度或更高精度表示，避免远景非线性深度量化失真；xy速度保持FP16。非有限/previous.w<=0/前帧UV或深度越界为无效；CPU验证有限输入，shader保护除法。
- jitter 使用8相位 Halton(2,3)，居中像素单位，clip.xy += (2*jitterPixels/extent)*clip.w，不修改投影深度。resolve须用 `previousRasterUV=currentRasterUV-velocity+previousJitterUV-currentJitterUV`。第一帧无历史，相机切换/重置、场景替换、resize、jitter开关清空。
- 太阳CSM始终用未抖动VP；主场景/天空使用抖动VP和匹配的逆矩阵；cluster深度基准仍未抖动，构造格子射线用抖动逆矩阵。相机剔除扩张半像素最大jitter边界。
- 天空只记录相机旋转的方向运动，忽略平移，previousDepth=1；清屏背景无历史。探针捕获使用独立临时第二目标、无历史和无jitter，且不改变正常历史。
- BLEND输出xy=0,z=0,w=材质alpha，并使用与HDR相同的普通alpha blend。覆盖后的有效度是背面有效度*(1-alpha)，M6-B必须拒绝低于阈值的结果；其xy/w不再是可信运动/深度。无需新增independentBlend功能要求。不透明和MASK关闭blend，保留完整运动；debug线同样减少有效度。这不等于已解决透明层TAA。

## Vulkan集成

UBO尾部追加当前/前帧未抖动VP、前帧相机及历史标记、当前/前帧jitterUV，旧字段偏移不变。push仍128B，尾部原shadowPass.y作为顶点运动索引；vertex-only binding10是80B/实例的previous-model+flags SSBO，fragment storage数量仍4、sampler仍16；pipeline layout总storage额度必须>=5、每stage storage>=4、fragment总resources>=22，在创建资源前由已有frame/material layout guard校验。这增加总storage最低要求，不能宣称覆盖Vulkan所有最低额度设备。目标 COLOR_ATTACHMENT|SAMPLED|TRANSFER_SRC，Graph增加按use顺序绑定至多4个颜色附件（Vulkan保证的最低额度），各目标独立记录内容/依赖；在Graph写后导出sampled；SSBO增长/rebind只能在对应槽fence结束后。swapchain候选资源全部成功后原子替换，并重置历史。禁止在探针捕获提交中发布正常历史。

## 可证伪验收

CPU验证序列、jitter投影和UV方向、未提交快照不前进、移除/重新出现/不同实例与材质复用、重置。GPU读回首帧/静止零速度、相机平移/旋转、同网格实例独立运动、非均匀/反射变换、MASK孔洞、透明有效度、天空平移零/旋转非零、jitter仍零几何速度且CSM矩阵不变。双pending槽验证独立buffer与前帧提交关系；scene/resize/reset失效，Graph barriers和资源归零。完整软件回归保证HDR/PBR/CSM/spot/probe/异步加载/WSI合同不回退。软件GPU数值不是4060 Ti性能或最终抗锯齿画质验收。

依据：RTR4第5章变换、第6章采样与抗锯齿；2020 [A Survey of Temporal Antialiasing Techniques](https://research.nvidia.com/labs/rtr/publication/yang2020survey/)明确区分采样累积和历史验证。2026-10-06通过agent-reach/Jina读取作者官方入口；这次只实现其必要输入合同，不声称完成TAA或使用某个重建SDK。

额度依据：[VkPhysicalDeviceLimits 官方定义](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceLimits.html)。2026-10-06通过官方refpage核实per-stage和pipeline-layout总额度分别检查；spec章节的Jina访问遇到验证页，采用可读取的官方refpage。
