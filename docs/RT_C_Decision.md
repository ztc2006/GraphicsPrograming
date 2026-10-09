# RT-C：基础玻璃、介质和光栅探针近似

2026-10-09。按已确认范围执行：实体/薄片、IOR/透射/吸收，焦散专项、嵌套介质和体积散射后置；用户保留视觉判断。

## 材质与内容

显式PBRT dielectric+eta识别厨房玻璃，不按名字或alpha推断；KHR_materials_transmission/volume/ior因子支持，首轮对不支持的透射/厚度贴图和相互排斥组合明确拒绝或保留原optional诊断，不宣称完整扩展贴图实现。IOR>=1且有限，transmission[0,1]，sigma非负。KHR attenuationColor/distance转为sigma=-ln(color)/distance；PBRT无吸收信息则sigma0，不编造材料厚度。KHR无volume/thickness0按薄片；PBRT实际闭合网格请求实体。

网格按原始位置精确焊接（只用于诊断，顶点/资产不改），边2-manifold/相反方向、各连通壳正体积验证；开口、非流形、反向/零体积回退薄片并提示。该验证不是一般自交证明；不补洞，不整体翻法线。入口同时暴露内容判定，实际GPU准备再次独立判定以覆盖其他加载调用。原始glTF/bin保持。

## RT光学

基础为光滑介质界面：准确介电Fresnel、Snell折射、TIR，单活跃介质识别出入界面与相机起始在内。透射radiance按eta_i/eta_t平方权重，RR使用eta补偿；几何法线与外向符号保留，不把RT-B面向相机法线当作介质外向。Beer按实际相邻界面世界距离，包含偏移补偿；毫米薄盒验证。

薄片为两界面无限薄模型，有效R=2F/(1+F)，净透射方向保持直线；只有作者给了薄片厚度才吸收。混合transmission的剩余分量为漫反射，透明光学不等于随机alpha洞；PBRT core转换alpha=.35被识别为光学转换近似，派生材质不沿用其coverage，真正KHR mask/blend coverage保持独立。光学界面须两面求交，避免漏掉出介质面。

单活跃介质，遇到重叠/嵌套不假装完整介质栈；诊断/保守边界明确。直接光采用逐最近交点的彩色直线玻璃可见性，非折射焦散解，不假装直线shadow rays等于完整曲折光路。面积/env MIS对delta reflection/refraction不使用连续pdf，透明路径至光源无双计。HDR/静止均值/slot隔离与成功submit发布保持。

## 纯光栅近似

首轮采用捕获探针折射/反射，读取同IOR/transmission/sigma，保留已有HDR/显示及独立模式。闭合实体的光栅光程仅为几何AABB/作者厚度的明确proxy，开口不猜厚度；捕获范围/静态更新/遮挡不可作为真实光路保证。无需调用RT或增加屏幕内attachment反馈。primary/双面渲染与透明路由同步，probe capture使用其明确简化分支，避免递归采自己的探针。后续屏幕空间背景折射作为质量增量，不强行夹进本轮。

原main608B/push128B/indoor1088B/Vertex112B保持；material UBO48->80B，RT实例144->176B，payload扩展。光学数据进入材质/历史key并由场景资源所有权负责，能力与容量回退保留。

2026-10-09 表面覆盖复查：光学coverage为Opaque/MASK时，探针近似已经包含背景，主画面复用双面不透明管线写深度，选最近表面，避免前后玻璃由三角形顺序覆盖。coverage BLEND保留混合；capture直接光opacity近似仍用透明管线。透明队列路由保持，TAA透明资格保持；双面和两种相反三角形顺序数值验证。

## 验证

CPU/Fresnel正入射(.04)/critical angle/Snell/Beer与能量，闭合/开口/接缝/反向壳/多组件与原导入语义；glTF因子/default/无效输入/贴图诊断、PBRT eta。GPU闭合薄盒真实透射/折射位移/内反射/吸收厚度/负缩放、薄片直行与光学alpha、shadow透光及粗糙参数边界、两帧/历史/失败/归零；光栅probe方向与同数据、capture非递归/alpha路由。厨房原资产审计和RTX全相关回归/1080p短测。不替用户评价画质，未接NRD。

## 依据

RTR4第9/14/23.11/26章；[PBRT dielectric](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF)，[KHR transmission](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_transmission)，[volume](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_volume)，[IOR](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_ior)，2026-10-09核对。volume说明ray tracing使用实际距离、raster thickness是有损近似；不将两者数值保证为相等。
