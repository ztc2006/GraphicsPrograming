# M2-A：线性 HDR 与统一显示输出的实施决定

2026-10-03。写图形代码前使用 `real-time-rendering-advisor`：已阅读 decision-playbooks、shading-texturing-pbr、lighting-effects-volumes，并用本地 RTR4 检索核对曝光定义（第 8 章，PDF 文件页 246）与透明合成（第 5 章）。用户已经确认 Linux/Vulkan/GLSL、4060 Ti、原生 1080p/60 FPS、静态场景和移动相机/光/物体，硬件验收延后。

## 决定与替代

选择单帧原生分辨率 RGBA16F 场景附件。场景、天空、mask/blend、3D debug 在线性空间累积，完成后单 fullscreen pass 处理全场景 EV 曝光与现有 filmic fit，再输出 SDR sRGB。ImGui 在显示变换之后绘制。

| 候选 | 取舍 |
|---|---|
| RGBA16F（选定） | 保留 HDR、alpha 和有符号诊断值，1080p 15.82 MiB；精度/范围与带宽适合首个实现，必须查询 attachment/blend/sampled 能力 |
| R11G11B10 浮点 | 更省带宽，但没有 alpha、蓝通道精度更低且无负值；后续在实测和画质对照支持时再考虑 |
| RGBA32F | 范围/精度更高，资源和流量翻倍；当前没有需要它的 scene 证据 |
| 直接 LDR 输出 | 难以在统一曝光前保留亮度和正确透明混合，不满足本阶段目标 |

继续使用原工程 filmic/ACES fit 数学近似，迁移到公共输出；不宣称完整 ACES 色彩管理。暂不引入新的 post-RTR4 算法、HDR 显示器/PQ、自动曝光、Bloom、TAA 或动态分辨率。

## 数据与同步

HDR attachment（ColorAttachment + Sampled）→ 颜色写入到 fragment sampled-read 的 barrier → SDR fullscreen 输出 → UI → present。输出按 native 像素 texelFetch，避免再次滤波。sRGB attachment 由 Vulkan 编码；UNORM/sRGB nonlinear surface 由 shader 用分段 sRGB transfer 编码，禁止重复 gamma。数据型 debug 绕过曝光和 tone mapping，保留编码。

环境亮度与相机曝光分开：environment intensity 影响天空/IBL；EV exposure 影响直接光、间接光、emissive 和天空的最终合成。继续使用现有 straight-alpha 的 src-alpha/one-minus-src-alpha 混合与排序；本轮不改材质 alpha 约定。

HDR 资源、view、采样器、输出描述符与 pipeline 作为 resize 候选整体构建，成功后交换；先完成旧帧使用才替换。VMA 账本跟踪额外附件与描述符，失败/退出归零。新增 GPU output 时间区间与报告字段，避免把显示输出藏在 main/UI 时间里。

## 可否证的验收

生产渲染器实际绘制已知 emissive 不透明/透明全屏面，回读 RGBA16F 验证 >1 未截断与在线性空间混合（半精度误差容限）。生产 display shader 对 sRGB/UNORM、曝光、tone-map 开关、分段 transfer 边界及像素方位进行离屏回读；允许 8-bit 量化的 2 LSB。检查合成后映射与映射后混合的结果能被测试区分。曝光只影响显示结果，不改变 HDR 附件；数据 debug 不被 tone curve 弯曲。

已有异步/共享/取消/退休与真实 ImGui 回归、resize 后绘制、账本统计与最终归零、报告 query 完整性继续通过。原生 1080p 四合院加载/绘制/退出和软件资源增量记录。GPU 时序/内存账本可检查，4060 Ti 帧预算与运动画质仍等用户换机，不用软件时间声称 60 FPS。

## 证据边界

- RTR4 第 5 章透明/合成、第 8 章场景参考颜色/曝光/显示、第 12 章图像空间处理，作为稳定基线；没有新增研究论文结论。
- 2026-10-03 核对 [Vulkan framebuffer 规范](https://docs.vulkan.org/spec/latest/chapters/framebuffer.html#framebuffer-blending)：浮点 attachment 的混合不做 UNORM 截断；sRGB 输出转为非线性 RGB，alpha 不编码。格式 blend 支持需要查询 COLOR_ATTACHMENT_BLEND；不能仅凭格式名称推断。
- 官方规范是 API 语义证据；本地回读是集成正确性证据，二者都不是硬件性能/最终画质验收。网页先按 agent-reach/Jina 路由尝试，未取得内容后用官方网页读取核对。
