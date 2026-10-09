# 延期的 BVH 与距离场路线

2026-10-08。用户要求将 BVH、SDF 等能力排在 AO 实现之后。
仍以Linux、RTX 4060 Ti 8GB、静态glTF场景及刚体/灯光移动为范围。光栅保持原生1080p/60FPS目标；新独立RT初版无硬FPS、正式对照1080p。
本文记录AO后的路线；GTAO/M7-A2现已实施并通过基础回归，见[M7-A2记录](M7_A2_AO_Quality_Implementation.md)。对象BVH/SDF尚未实施，已按2026-10-09指令延期；RT-A/B/C已实施，下一项RT-D降噪。

2026-10-09 新调整（用户显式grill-me）：实时光追前置；独立RT与光栅分别建管线，共享场景/材质/光学参数/灯光/相机/曝光。首轮阴影/反射/多反弹GI、基础实体玻璃；开口玻璃明确薄片回退，不补洞；焦散专项/嵌套介质/体积散射后置。成熟降噪库（首选NRD），玻璃透射另行处理；主画面优先降噪，允许静止累积，初版RT无硬FPS，正式1080p；厨房+解析场景。UI首轮仅切换渲染方式，分屏/分割线/成对截图工具后置。用户先判断画质，助手仅被明确要求时评价。硬件BLAS/TLAS随RT建设；CPU对象BVH/Hi-Z/indirect/LOD和SDF延期。用户“推进光追管线”已确认完整方案及RT Pipeline＋SBT，见[实施清单](RT_Comparison_Plan.md)；RT-A/B/C/D依次为基础+切换、光传输、玻璃、降噪与验收。RT-A已实施验证，见[记录](RT_A_Implementation.md)；下一项RT-D。

## 原顺序（被以上调整替代）

M7 GTAO → M8 BVH/Hi-Z 与按测量接入 indirect/LOD → M9 网格 SDF/距离场软阴影
→ M10 多探针/SSR。原 M8 反射阶段顺延为 M10；之前提前实现的单房间探针及当时 M8-A
记录仍保留。BVH/SDF 不插到 AO 前；剩余 TAA 运动验收继续与 AO 的运动回归衔接。

## 当前架构与术语

当前代码对每个世界 AABB 做平面测试，没有树。AABB 是轴对齐包围盒，BVH 是包围体层次结构；
用 AABB 作节点的树也是 BVH。优化方向是把扁平查询组织为层次，而非淘汰 AABB。
PBRT 的 [BVH](https://pbr-book.org/4ed/Primitives_and_Intersection_Acceleration/Bounding_Volume_Hierarchies)
使用 [轴对齐 Bounds3](https://pbr-book.org/4ed/Geometry_and_Transformations/Bounding_Boxes)。

厨房约299个绘制对象、144万三角形；当前 GPU 主通道成本主要随像素量变化。
对象 BVH 减少候选查询，Hi-Z 处理视锥内遮挡，LOD 降低几何工作，三者分别验证。
BVH 不保证每个场景都比扁平扫描快，也不能消除可见像素的着色成本。

## M8：层次可见性

- A：对象 BVH 的静态构建、脏变换 refit、退化重建和紧凑线性节点；比较中位数划分/binned SAH。
  将原始实例/世界 bounds 构建成候选树，随场景原子提交；失败保留旧树。
  查询输出稳定 ID，保留 opaque/mask 顺序及透明排序；完整实例继续服务运动历史、探针和退休。
  未知/非法 bounds 保守保留；小场景有扁平参考/回退。第一版不引入 GPU pass。
- B：Hi-Z 遮挡与实际减少绘制的路径。先明确深度来源、保守投影和历史失效/显露回退。
  GPU indirect 必须解决当前 per-object push、网格 buffer 与材质绑定限制，纳入实例表、
  间接命令及批次组织；不逐物体同步读回或 queue.waitIdle。
- C：依据几何/CPU提交瓶颈评估 LOD/网格重排；若仍是像素瓶颈，不以更少三角形宣称提升。

验收：300/1k/10k/50k实例、不同空间/遮挡分布；构建/refit/遍历/候选数/峰值内存与整帧成本。
大场景查询p95要有收益，小场景避免退化；与扁平路径HDR/阴影一致，漏剔除零容忍。
初始对象树预算为50k实例≤16MiB，不含原始几何和临时构建峰值，这是设计目标。
Hi-Z覆盖快速转身、移动遮挡物、遮挡显露、mask/blend、camera cut；透明默认保守。
CPU对象BVH与未来 Vulkan硬件RT的BLAS/TLAS分开，后者仍有独立构建/更新/同步合同。

## M9：网格 SDF 与距离场效果

- A：SDF明确指网格有符号距离场。先做离线/后台生成、版本化缓存、3D资源上传和
  距离/符号/梯度/sphere-tracing可视化。实例复用局部场；缓存身份含内容、分辨率、单位和算法/符号策略。
  闭合网格承诺符号；开放/薄片/non-manifold明确诊断及几何回退。
  体素插值、步进、镜像和非均匀缩放必须保守，不能把局部距离当世界距离。
  R16F/R32F在实施时检查格式/精度；初始驻留预算128MiB。
- B：一盏主要光源的距离场软阴影原型，处理屏幕外遮挡和较宽半影。
  近处细节沿用已有阴影，按距离/可靠度组合成一个visibility，避免重复计遮挡。
  受影响对象列表/局部场域先行，全局clipmap在规模测量需要时扩展。
  新GPU资源/pass在实施前写明单队列图依赖、精度与回退。

验收：解析球/盒与三角形最近距离、符号、表面误差≤一个体素的初始目标，另验证保守步进。
薄墙、掠射、缺失场、刚体移动、尺度及接触/半影变化独立检查。
第一版效果额外GPU p95≤1ms为目标，与既有阴影共同服从原GPU14ms/帧16.7ms预算；
不能把效果预算简单累加而提高总预算。未达标时按效果范围/质量档重新比较。
SDF是几何表示，不自动提供动态GI；远距离DFAO、全局场和GI继续独立评估。
若与GTAO配合，按尺度/可信度组合，不能无条件叠乘把间接光涂黑。

## M10 及以后

继续多探针覆盖/混合，再SSR和probe回退。各向异性BRDF、玻璃transmission/volume/IOR
列为AO后的材质扩展候选；已下载参考资产，不宣称当前已支持。
独立RT/多反弹GI/实体玻璃已前置到RT对照阶段，不再延期到BVH/SDF之后。全面bindless、异步队列和虚拟几何仍须独立预算/内容需求，不塞进BVH/SDF增量。

## 依据

RTR4第18/20/22/23章用于可见性/有效着色/保守测试/性能，第14章用于距离场表示。
PBRT以上章节用于BVH原理/构建对照，其光线SAH和离线速度不等于本项目视锥收益。
Epic的 [Mesh Distance Fields](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-distance-fields-in-unreal-engine)
及 [Distance Field Soft Shadows](https://dev.epicgames.com/documentation/en-us/unreal-engine/distance-field-soft-shadows-in-unreal-engine)
提供生产实现用途与限制参考；本项目不直接继承其API/旧硬件耗时或全部限制。
来源核对日期2026-10-08，以上预算均未经过新算法实测。

2026-10-09：RT-B光照/阴影可见性/反射/多反弹GI与均值已交付验证，45/45原生；物理玻璃和成熟降噪分别为RT-C/D。见[记录](RT_B_Implementation.md)。

2026-10-09：RT-C基础玻璃与光栅同参数探针近似已实施；闭合实体/开口薄片明确诊断。下一项RT-D成熟降噪与独立透射处理，见[RT-C记录](RT_C_Implementation.md)。
