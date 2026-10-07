# M5-A 稳定太阳级联阴影（2026-10-05）

实现前按实时渲染技能记录了[选择与验证合同](M5_A_CSM_Decision.md)。
本轮提供软件实现与回归，4060 Ti画质/性能/RenderDoc验收仍按用户要求延后。

## 行为和资源

默认四级CSM，可切换1/2/4级或旧单图。实用线性/对数分割、视深度选级、
重叠区PCF混合，最后一级渐退到无阴影的直接光。默认距离60世界单位并受相机far
限制，lambda=.65、混合比例.1、投影者深度扩展50世界单位。

从标准透视VP反投影Vulkan深度0/1；CPU双精度求角点/球形包围，量化半径，
在固定世界光基中按世界texel对齐中心。覆盖PCF4和对齐余量，避免旋转时使用
紧贴AABB造成缩放抖动。正交、奇异、oblique/不一致快照退回旧单图；非法CSM
设置在acquire/reset fence之前拒绝，正常重试仍可运行。

复用4096×2048 D32图集：左半四个1024平方sun tiles，右半spot布局不变；
CSM关闭后左半恢复旧2048平方单图。阴影image/view/sampler和13套现有流水线
不增加，图集payload仍32MiB。每帧Shadow/probe SSBO从608B增加到1040B：
每槽+432B；448B UBO、128B push与fragment的16 samplers/4 storage buffers不变。
在本槽fence完成后更新，Graph保留host→vertex/fragment和depth→sampled屏障。
没有新增运行时上传/fence等待，完整opaque/MASK投影者每级绘制，透明不投影。

CSM偏移以世界常量（默认.003）和世界texel斜率（默认1.5）设置，按级联深度跨度
换为比较深度。接收面世界位置导数在discard和级联选择之前取得，每级将导数变换
并求UV→depth梯度，PCF按实际tap位移校正比较深度；另覆盖硬件双线性半texel足迹。
奇异UV Jacobian退回基本偏移。CSM关闭固定光栅偏移，旧单图/spot保留1.25/1.75，
通过动态DepthBias复用旧单面/镜像/双面管线。tile clamp保留，采样不会串到spot。

房间探针六面捕获继续使用固定场景单图，与主相机级联无关；改变CSM距离/档位
不会无理由使探针变旧，原有灯光/材质/几何刷新合同不变。

## UI 和报告

Lighting → Sun Shadow：Stable CSM、Cascades、Shadow distance、Split lambda、
Cascade blend、Caster extension、Bias world units、Bias texels、Sun PCF Radius。
Inspect cascade coverage（PBR Debug=Sun Cascades）显示级联颜色和过渡；
Depth debug cascade选择原深度模式使用的tile。关闭Stable CSM可调整旧单图参数。
数据debug绕过曝光/filmic，保持正常显示颜色编码。

benchmark schema4追加sun_cascade_count与sun_shadow_distance。字段来自最后的
主视图配置，在finishBenchmark取得，避免启动探针的单图状态被误记为主视图CSM。

## 软件验收

Release/Ninja；显式llvmpipe/X11与MESA_VK_WSI_DEBUG=sw,noshm，逐项串行运行。
生产源码指纹6084d53a8d16654c00179687664fc7609ff8e1bc074db1a6cb66ec4d75cabd0e
（95 inputs），与生成的build_info一致。完整CTest **28/28**（17CPU/11软件GPU），
346.07秒。新CSM CPU检查另通过ASan/UBSan，shadow.vert和triangle.frag通过
Vulkan1.3 SPIR-V验证。软件耗时不作为目标硬件预算。

独立生产管线HDR读回（一/两帧）：

| 情况 | 结果 |
|---|---:|
| 四个级联分别无遮挡 / 遮挡 | 每级0.318115 / 0 |
| 两级不同遮挡，混合权重.25 / .5 / .75 | .249878 / .499756 / .75 |
| 30°接收面，PCF0 / 1 / 4 | 每档.275635，无自阴影暗斑 |
| 最后一级渐退中的可见性 | .535156 |
| 阴影距离外 | 1，直接光保留 |

斜面测试先在PCF4读回.205078（无阴影参考.275635），校正后.275635；
证明这是深度比较问题，未通过加大全局bias解决。早期不同遮挡测试的三角形
自带z=.5，导致采样点没有进入预期混合带；将独立fixture归零后才采用其结果。

还验证视锥外上游投影者、移走后的更新、sun关闭与spot共存、旧单图对照、
非法设置不提交/正常重试、每级完整投影者列表、级联原始颜色/显示设置。
未来timeline信号同时阻塞两槽，分别提交4/2级、距离10/40，并读回蓝/红级联色，
确认1040B SSBO和descriptor独立且query/退休/最新ID不受影响。
现有MASK、镜像、mip/IBL、84组full/clustered完整HDR对照和异步加载回归保持通过。
实际viewer启动（640×480，warmup0/duration5，正确性烟测）：完整厨房auto一槽4rows，
四合院auto两槽4rows，完整厨房asset两槽1row，共9个有效GPU query rows。
全部实际启用四级/距离60；厨房auto保留两盏spot阴影和有效probe，另两次无局部灯/probe。
源码指纹一致，present release proven，prepared/retired/staging全部归零。
这些短烟测不作为帧率或最终画质验收。
日志、源码manifest与本轮独立补丁保存在本聊天outputs/csm。

## 边界和后续

固定尺寸球形拟合会留空白区域，最远处受阴影距离和投影者扩展限制；相机大幅
旋转会改变覆盖中心，纹素对齐不等于任何运动下阴影完全静止。FOV/aspect/near/far、
档位和split变化会重建拟合；本轮尚无TAA。PCF接收面校正依赖局部平面近似。
投影者剔除、可调图集分辨率、PCSS/点光阴影均留给需求/测量支持的后续增量。

本机首次构建曾出现Ninja dyndep断言；两次构建曾短暂重叠。此后构建串行，
在无C++模块的工程中关闭本地CMAKE_CXX_SCAN_FOR_MODULES并重建，工具日志保留。
依赖日志出现截断恢复提示，做了recompact；未据此宣称增量构建速度验收。

4060 Ti后补固定相机轨迹、级联交界/远瓦/薄墙双面叶片、太阳角度与移动物体、
接触脱离和acne、validation/RenderDoc、真实GPU时长与VRAM。下一项M6-A：
相机/物体前帧变换、运动矢量和jitter合同；随后接TAA解析与历史拒绝。GTAO仍为M7。
