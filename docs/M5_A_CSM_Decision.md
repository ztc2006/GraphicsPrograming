# M5-A 稳定级联阴影决定（2026-10-05）

按已确认路线继续：Linux/Vulkan、静态 glTF 场景允许相机/灯光/物体运动，
原生1080p/60FPS为待实机验收目标；本轮只使用显式软件Vulkan，不探测物理GPU。

## 选择与依据

采用四级稳定级联阴影（Cascaded Shadow Maps, CSM），实用线性/对数混合分割，
视深度选级、重叠区双级PCF混合、最远处渐退。允许1/2/4级和关闭CSM的旧单图对照。
RTR4第7章作为采样/偏移/过滤基础；四级不是帧预算实测结论。
固定全场景单图无法兼顾近远采样；紧贴光空间AABB会随旋转改变尺寸，稳定性不足。
球形包围、固定尺寸和纹素对齐牺牲部分图面积，换取固定相机参数下的稳定采样。
VSM增加格式/过滤和漏光变量，PCSS增加采样预算，均后置。

2026-10-05通过Agent Reach/Jina及官方原文复核：
[Microsoft CSM](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps)
提供分割、稳定投影、混合及PCF边界依据；
[阴影深度技术](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/common-techniques-to-improve-shadow-depth-maps)
说明偏移、分辨率、裁剪和接触分离的取舍。其D3D示例不直接承诺Vulkan性能。

## 资源、数据与同步合同

复用4096×2048 D32图集（32MiB payload）：太阳左半四个1024平方tile，
聚光灯右半布局不变；旧单图模式仍占左半2048平方。无新增image/sampler/descriptor，
帧SSBO追加四矩阵/rect/bias、split、前向近面及参数；448B UBO和128B push不变。
每槽fence完成后写其独立SSBO。现有Graph的host→shadow vertex/main fragment和
depth attachment→sampled depth屏障复用；不修改pending描述符，不增加运行时上传等待。

CPU使用双精度反投影Vulkan NDC z=0/1，提取标准透视near/far/forward；
不一致、奇异、正交/oblique快照退回旧单图对照，非法CSM设置在acquire前拒绝。
距离受相机far限制；级联半径量化并覆盖PCF4及半texel对齐余量；光基固定在世界坐标，
投影中心按世界texel对齐，避免随camera eye平移光基。
深度范围扩展可调投影者距离，包括视锥外、在光源方向上的投影者；本轮完整绘制
全部opaque/mask，保留双面/负缩放/coverage。超出扩展距离的投影者不保证覆盖。

偏移以世界常量及世界texel斜率设置，除以各级光深度跨度后送shader；PCF4限制在
各tile的半texel边界内。斜面PCF4回读从0.275635降到0.205078，证明邻近
采样需要接收面深度校正。dFdx/dFdy在discard/级联选择之前取得世界位置导数，
每级线性变换并求UV→depth梯度；每个PCF tap按其实际UV偏移校正比较深度，
额外覆盖硬件双线性半texel足迹。奇异UV Jacobian退回基本偏移。
CSM关闭固定光栅偏移，仅使用上述世界单位偏移与接收面校正；旧单图和spot保留
原光栅1.25/1.75设置。通过动态depthBias复用三套阴影管线。
不提高全局bias来掩盖acne。按视深度选级，后一图向前
扩展覆盖前一级混合带，最后一级渐退为可见，远处直接光仍保留。

探针六面捕获继续使用独立固定场景单图，避免视点级联影响捕获一致性；其按需刷新
合同保留。主视图采用CSM。提供级联色、原始指定tile和可见性调试，场景切换保持相机
更新，resize改变aspect时重新构造级联。

## 可证伪验收

CPU：1/2/4分割单调、near/far与混合覆盖；所有八角z∈[0,1]及xy覆盖；
垂直太阳、亚texel移动矩阵稳定、旋转半径稳定、PCF安全边界、非法输入与回退。
GPU：生产阴影/片元管线的每一级遮挡与无遮挡，混合带与远端渐退数值，
视锥外投影者、移动投影者、级联debug、sun关闭/局部灯共存、旧单图对照，
一/两帧快照及现有MASK/镜像/资源生命周期回归。
运行完整厨房/四合院并记录实际级联、query与释放结果；软件数据不做性能保证。
4060Ti后补固定慢速平移/旋转视频、交界、薄墙/叶片、太阳角度、接触与acne、
RenderDoc/validation、GPU时长和VRAM，目标预算见路线。
