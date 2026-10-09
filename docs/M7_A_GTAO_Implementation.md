# M7-A：GTAO 基础通路实施

2026-10-08。本轮已实现，可运行；决策与数据/同步合同见 [实施前决策](M7_A_GTAO_Decision.md)。

## 行为

主场景从 HDR/motion 两个颜色附件扩为 HDR/motion/indirect diffuse 三个RGBA16F附件。每个fragment初始化第三输出；正常不透明/MASK PBR输出已乘材质AO的diffuse IBL，debug/unlit/天空为零。BLEND自身输出零RGB并按同alpha衰减底层间接漫反射；不新增透明深度/遮挡。探针用临时第三附件仍只捕获直接光，不执行AO。CSM/cluster/主UBO608B/push128B不变。

三个独立fullscreen通道在HDR后/TAA前执行：3切片×每侧6步GTAO horizon积分→5x5空间滤波→HDR减去`diffuse*(1-visibility)`。D32深度使用真实jittered inverse raster VP重建世界位置；选择较近相邻导数重建朝向视点的几何法线，跳过屏幕外/天空，不夹边重复遮挡。世界半径默认.5、距离.6R→R平滑衰减、最大128px采样跨度；固定像素噪声，无独立时间AO历史。AO参数变化清TAA颜色，未变化值保留。Raw/Filtered debug关闭TAA和曝光/filmic，普通PBR/debug/探针遵循原合同。

viewer默认打开，兼容Renderer API默认关闭。UI/CLI均提供开关/世界半径/strength/raw/filtered；每槽6个query分别统计horizon/filter/composite以及合计。独立AO push112B、单descriptor set5个绑定（单shader最多4个实际samplers），所有描述符构造后不再改写。TAA为原始HDR和AO composite分别保留两种parity immutable sets；display可以选择composite。单graphics queue共享目标，Graph处理RAW/WAR/WAW；状态只在submit成功后发布，resize先drain。新目标20B/pixel：1080p41,472,000载荷字节（约39.55MiB），resize总目标64B/pixel；不是driver heap。AO关闭跳过三个pass，MRT/目标仍存在，不能把off/on差视为全部新增资源开销。swapchain重建新增pipeline统计13→16（另有cluster1）。

## 验证结果

最终生产源指纹 `82e3d4c88a29bb484a3c5c027819259c63467a7b5e107d9153e9c66728608807`。Release构建通过；六个变更fragment SPIR-V通过Vulkan1.3校验；差异空白检查通过。

RTX4060Ti/NVIDIA615.71.09/X11 KHR present-fence原生CTest **36/36，74.79s**（19CPU/17GPU），包括legacy/EXT、加载失败/连续切换、probe/CSM/cluster/材质/motion/TAA/resize/ledger回归。一次较早全套运行被外层125s总运行限制终止；不计为通过，原始日志保留。修正旧离屏fixture的第三MRT后，重新跑完整36项，每项仍保留90/120s原超时。

64×64解析深度GPU fixture：无遮挡平面最低.979492、contact最低.578125；sky=1、world小半径不误遮挡、零strength HDR精确不变、raw debug精确匹配、最大radius/strength有限；每像素只从已知diffuse项减光。实际Renderer验证AO/TAA immutable source selection、参数变更/未变化值、debug/off、七个完成queries及重建；1/2槽均通过。增强已有timeline gate：两个AO提交同时pending，raw白/HDR.25保持独立push，六query pools独立，完成前没有回调，完成后全部一次交付，旧场景/预览保活和最终ledger归零。

完整厨房（293opaque/6blend）实际942×1012回读：off/on、raw/filtered、zero-strength、probe重捕获隔离均通过；RGB下降总78546.3，>0.002变化的通道1,026,678，未检测到>0.004的增亮。固定目标/正弦横移慢.08、快.8世界单位，各24帧，AO+TAA全部queries有效并导出序列。检查静止对照和慢/快接触表，墙角/柜体接触可见；这48帧不能证明分层透明/移动灯/所有薄物体/镜面最终运动质量。

### 原生1080p短测

各5s预热/10s采样，Release、正常无capture注入；窗口始终在活动workspace，完整CSV保留所有慢帧。

| 场景/设置 | 完成GPU帧 | total p50 ms | AO p50 ms | horizon/filter/composite p50 ms | slow acquire帧 |
|---|---:|---:|---:|---|---:|
| Kitchen AO off /2slots/TAA | 279 | 11.015 | 0 | 0/0/0 | 7 |
| Kitchen AO on /2slots/TAA | 691 | 8.330 | 1.204 | .903/.248/.052 | 5 |
| FlightHelmet AO on /1slot/orbit/TAA | 1800 | 1.779 | .389 | .236/.103/.051 | 0 |

**off/on整帧差不能作AO收益或因果成本证据**：main成本8.847→4.918ms及约995–999ms acquire慢状态表明运行条件不稳定，未锁定原因或GPU时钟。独立AO query只记录该运行中的实际通道成本，不转移为稳态保证。三次共2770个有效query frames，pending presents/prepared/retired/staging均零。没有validation layer；Wayland、正式30s预热+3×120s长测、完整GPU预算仍未验收。

## 后续

基础M7-A已交付，下一项为薄物体/屏幕边缘/尺度与更广AO参考质量对照；保持M6-C剩余透明/移动灯/高光轨迹待办。只在成本/质量证据支持时加入depth mip、half-resolution或独立时间AO。Bloom/自动曝光尚未实现；对象BVH/Hi-Z→SDF仍按用户路线排在AO后。未提交/未推送。

本次证据：`/home/ztc233/Documents/Codex/2026-10-08/home-ztc233-vulkan-graphics-graphicsprograming-ao/outputs/m7a/`，包含全量日志、原始HDR、对照PNG、48帧PNG/两段GIF、profiles完整CSV/JSON与相对起始dirty tree的patch。
