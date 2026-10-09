# M7-A2：AO 常数能量与单像素前景修正

2026-10-08。已实现并在RTX4060Ti验证；[决策、反例与取舍](M7_A2_AO_Quality_Decision.md)。这轮完成基础平面/薄物体/边缘/尺度参考检查，仍不代表所有材质、曲面或运动质量的最终接受。

## 两个具体问题与改动

1. 原3-slice有限方向求积将无遮挡平面误暗：正视/30/60/75度最大误差2.05%/2.78%/7.13%/5.71%。现在用相同slice的无遮挡余弦积分作分母：`sum(weight * occludedArc) / sum(weight * (cos(n)+n*sin(n)))`；后者来自将同一解析积分horizon设为`n±π/2`。不会把有遮挡的结果强行设白。最大平面误暗降至.000488281（约.049%，R16F量级）。
2. 真正1px宽box的front face原来会把两侧背景作为表面斜率，空间滤波输出最低.809082。现复用既有四邻域NDC depth，用单边差和对侧斜率相消残差检查连续性。硬件NDC深度在屏幕平面上仿射，平坦/倾斜表面成对差相消，孤立前景的两侧背景不相消。可信x/y轴不足时raw AO=1、filter保留raw；不以view-facing假法线继续积分。最终1px front最低1.0；2px front=.999512。保守回退可能漏掉未知法线像素的真实AO，尤其极薄/屏幕/clip边界。阈值`max(2e-6,.02*(1-depth))`是forward-D32约定下的项目启发式，不是完整几何法线恢复。

法线检查初次候选虽将1px front提高至.995117，但60/75度平面边缘重新误暗.03369/.06543；可信度回退修复后才通过所有gate。旧shader及中间失败日志/数据均保留；不把中间36项通过替代最终改动后的复测。

没有新pass/目标/sampler/descriptor/query/历史；主UBO608B/push128B、AO112B/20B-per-pixel、36 horizon samples、空间5x5以及成功-submit图状态/双槽合同保持。center depth由caller传入，连续性检查不增加texture fetch；未知法线early return减少该像素的无效积分/滤波。材质AO、直接光/emissive/specular隔离及probe direct-only仍沿用M7-A合同。

## 独立参考与检查

64×64 depth由camera ray/plane/闭合box解析相交生成，AO reference由2048个确定性cosine-weighted hemisphere ray/box相交计算；.6R→R平滑距离衰减与本项目近场定义一致。独立参考不共享shader horizon/max/slice实现；8192-ray复核的最大reference差=.0025956。统计主要在receiver平面；front face单独检查，未将所有box侧面当成已完整参考。

| 场景 | 旧filtered receiver MAE | 最终MAE | 接触带旧→最终MAE |
|---|---:|---:|---|
| 厚box | .0077967 | .00429097 | .0286447→.0284000 |
| 2px薄box | .0068811 | .00354208 | .0296201→.0294209 |
| 贴屏边box | .00571111 | .00289114 | .0299155→.0319155 |
| 屏幕外box | .00474275 | .00182378 | .0426780→.0473945 |

厚box所有receiver像素平均误差约降45%；接触带改善较小。贴边接触带略变差，屏幕外仍丢失遮挡；整体MAE下降不等于这些缺失证据已解决。1px用归一化中间版本作反例（front .809082→1），单独数据位于normalized目录。所有值有限且[0,1]、sky白；0.1/1/10统一世界尺度最大差=.000488281。16帧固定慢/快横移+8相jitter的60度无遮挡平面最大误差=.000488281；它隔离AO几何采样，不替代最终TAA运动质量接受。

最终原生X11/NVIDIA615.71.09/KHR present-fence **36/36 CTests，52.84s**（19CPU/17GPU）：新参考gate、双slot/pending AO push和query、probe/材质/CSM/cluster/motion/TAA、resize/加载/退休/最终ledger零；三个GTAO SPIR-V Vulkan1.3通过，增量diff空白检查通过。最终源指纹 `eb585b0666a14ae3db4bc0d0d28c070ea201ef9369e097eced08161f594bbf6b`。

实际厨房942×1012最终off/on/raw/filtered/zero-strength/probe-isolation和48帧固定慢/快AO+TAA序列通过，RGB下降总80631.2、> .002改变通道1,006,013；与前一版同extent/曝光/相机对照已导出。厨房图像来源Country Kitchen（Jay-Artist，CC BY3.0），原资产/许可在项目中保留。

## 原生1080p短测

Release、5s warmup/10s sample、无capture注入，所有raw frames保留。普通测试窗口一直在active workspace，pending presents/prepared/retired/staging均零。

| 场景 | 完整GPU samples | total p50 ms | AO p50 ms | horizon/filter/composite p50 ms | slow acquire帧 |
|---|---:|---:|---:|---|---:|
| Kitchen /2slots/TAA | 1147 | 8.292 | 1.207 | .899/.257/.050 | 0 |
| FlightHelmet /1slot/orbit/TAA | 1800 | 1.824 | .402 | .245/.106/.052 | 0 |

本轮两次短测没有>20ms acquire；不能据此宣称已修复之前~1s呈现慢状态，亦不能把跨轮时间差当成因果提速或正式60FPS验收。validation layer/Wayland及30s warmup+3×120s正式协议仍待。

## 下一项

AO实现与本轮基础质量gate已交付，可按用户“AO后BVH/SDF”顺序进入 **M8-A CPU对象BVH**：紧凑AABB节点、稳定ID/原绘制顺序、场景原子提交、脏bounds refit/重建、小场景扁平回退、未知bounds保守保留，先不加GPU pass。比较300/1k/10k/50k规模及主/阴影HDR不变。M6-C移动灯/分层透明/镜面最终轨迹、M7更广内容调优与Bloom/自动曝光继续独立待办，不伪称已完成。

本次代码尚未commit/push；既有未提交内容保留。证据：`/home/ztc233/Documents/Codex/2026-10-08/home-ztc233-vulkan-graphics-graphicsprograming-ao/outputs/ao-quality/`。每个`.reference_raw_filtered_depth.f32`是64×64×4 float32，通道顺序为reference/raw/filtered/depth；PNG、完整日志、CSV/JSON、原始厨房HDR和48帧PNG/两段GIF可复核。
