# M7-A2：AO 几何与边界质量检查

2026-10-08。继续已交付M7-A，不启动BVH/SDF。使用real-time-rendering-advisor的图像空间/间接光和诊断流程，RTR4第11–12章作AO证据边界；核对上轮保存的 [XeGTAO 作者积分/深度边缘源码](https://github.com/GameTechDev/XeGTAO/blob/master/Source/Rendering/Shaders/XeGTAO.hlsli)。资料是已归档作者实现，不宣称新SDK支持；新改动以本项目参考实验为证据。

保持RTX4060Ti、1080p、单队列/1–2槽、主UBO608B/push128B、独立AO112B、现有四目标20B/pixel、只调制间接漫反射。约1.204ms厨房AO仅为短测成本；慢acquire和最终性能验收独立待办。

先建立实际GPU深度回读的64×64参考场景：0/30/60/75度无遮挡平面；闭合厚box与1–3pixel薄box；贴屏边及屏幕外box；0.1/1/10统一世界尺度。每个case的depth来自独立camera ray/plane/box解析相交，参考AO来自确定性cosine-weighted hemisphere ray/box相交，并匹配有限R及.6R→R平滑衰减。与GTAO的方向slice/max-horizon代码无共享算法，避免把实现当参考。物体隐藏侧面、屏幕外和亚像素几何的差异属于可观察信息边界，分别记录。

预先设立检查：所有无遮挡平面raw/filter与1差≤.005（旧正视平面最低.979不满足）；薄box迎相机平面保持≥.95，不可因背景邻点造假法线而变黑；同屏depth/形状/半径按尺度同比缩放，AO最大差≤.006；所有结果有限且[0,1]，sky不染色/不重复夹边。闭合box的参考平均误差目标≤.12，记录contact-band误差及missing-screen-reference误差；该阈值只是基础近场质量闸门，不是所有内容质量认证。

若误差来自少数azimuth slice的无遮挡积分偏差，比较新增slice成本与对同slice无遮挡积分做归一化；仅以参考/接触质量证据选择。若薄物体法线跨背景，比较增加真实几何法线MRT（资源/主通道代价）与depth局部连续性检查，优先验证不加目标的连续性回退。空间滤波必须避免跨深度台阶把背景遮蔽涂到前景。

生产改动只在旧shader的新测试确实失败后实施，并保留old/new数值和图像。随后完整原生36项、pending双槽、SPIR-V/resize/probe/ledger合同复测；重新导出厨房对照及固定运动序列。性能用独立AO queries，保留所有慢帧，不以随意总帧差宣布提速。AO独立时间重用、Bloom/自动曝光和完整M6-C透明/移动灯/高光最终验收仍为后续任务。

## 实施前诊断结果

旧shader新fixture确实失败：正视/30/60/75度无遮挡平面最大误暗分别2.05%/2.78%/7.13%/5.71%；不是新物体接触。厚box filtered receiver MAE=.00780/contact=.02864，thin=.00688/.02962，edge=.00571/.02992；front top最差.99365，scale最大差.000488。当前证据不支持修改深度法线或增加法线资源；先只修正无遮挡积分有限azimuth求积偏差，保持pass/资源/采样数/法线不变。

选择同slice无遮挡积分归一化：当前`Σ L_proj*arc_occluded / slices`替换为`Σ L_proj*arc_occluded / Σ L_proj*(cos(n)+n*sin(n))`。后者是将相同解析arc边界设为`n±π/2`得到的无遮挡积分，不是将真实遮蔽归零或用当前图像校准；它消除可直接证伪的常数能量偏差。保持独立ray/box reference与旧shader指标，若contact误差显著变坏则撤销。未引入独立历史/额外fetch/MRT，仅增加每slice分母累积与最后一次除法。

## 单像素追加诊断与第二项修正决策

归一化版四倾角/16-frame平移+jitter最大误差均.000488281；厚box filtered receiver MAE .007797→.004291，但追加真正1px宽box时，其迎相机表面filtered最低.809082（应≈1），新gate确实失败。原2px box最低.999512，不能代替1px测试。保留归一化中间版本/数值，不把中间通过误报为最终接受。

选择不增加fetch/目标的邻域深度连续性检查：对每个轴，读取现有左右/上下四邻居；比较单边NDC深度差与两侧差的平均残差，使用两者较小值判定连续。平面在屏幕上的硬件NDC depth是仿射量，斜平面的成对差相消；孤立像素两侧都是背景时不会相消。无足够可信轴时沿用朝视点法线回退。threshold=max(2e-6,.02*(1-depth))是本项目启发式，须用同倾角/世界尺度/边缘/接触reference重新验证，不宣称统一几何真法线；深度quantization/极远处/曲面仍有限。此方案复用已有4个depth fetch，112B push、目标/描述符/同步不变。若真实接触漏遮挡明显增加，再改为额外邻域或几何法线MRT，而不是无证据增加带宽。

第二项初次候选反例：1px top提高至.995117，但60/75度开放平面边缘又出现.03369/.06543误暗。原因是未通过连续性检查时用view-facing法线继续积分；在斜面与sky/clip边界，这个法线没有足够几何支持。最终候选显式输出法线可信度：可信x/y轴都具备时才积分/滤波，否则raw=1、filter保留raw，不再拿fallback view normal制造遮蔽。无额外fetch（center depth由caller传入）、pass/目标/ABI不变。取舍是未知法线的极薄/边缘像素可能漏掉真实AO；该缺口通过white fallback可见，不以错误几何换伪细节。继续同参考与斜面运动gate验证。
