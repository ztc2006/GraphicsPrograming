# M2-A：线性 HDR 与公共输出合同

2026-10-03。实施前按用户要求使用 `real-time-rendering-advisor` 核对 RTR4 的透明合成、曝光与颜色基线、Vulkan 格式/混合语义、候选格式与可否证的测试。详见 [决定与依据](M2_A_HDR_Decision.md)。下一增量是 M2-B 最小单队列 Render Graph。

## 帧中的数据流

```text
shadow depth → 主场景 RGBA16F + D32（sky/opaque/mask/blend/debug）
             → HDR color-write / sampled-read barrier
             → 全屏 EV / filmic fit / sRGB 输出 → ImGui → present
```

`HdrOutput` 拥有原生分辨率 `R16G16B16A16_SFLOAT` 图像、view、独立 nearest sampler、描述符和输出 pipeline。`GpuImage` 管理 VMA resource/allocation 生命周期；格式必须支持 color attachment、blend、sampled 与诊断 transfer-src。图像每像素 8 字节；1920×1080 为 16,588,800 字节，即 15.82 MiB。

场景和天空 shader 输出线性辐射值。保持现有 straight-alpha 约定、透明排序和深度策略，在浮点附件中执行 `srcAlpha / oneMinusSrcAlpha` 合成。颜色附件必须 store 供后续采样；当前主深度仍不保留，M2-B 再声明其用途与状态。一个在飞行的帧、单队列、原生分辨率保持当前约束。

`HdrOutput::prepareScene` 转为 color-attachment，`drawDisplay` 在结束场景 rendering 后转为 shader-read，随后开启目标 rendering 并留下给 UI。Renderer 负责 swapchain 状态、目标格式/extent、rendering 边界与资源释放前的 GPU 完成。图像状态跟随已记录帧；新图从 undefined 开始。

公共输出使用 `texelFetch(gl_FragCoord.xy)`，避免缩放、额外过滤或上下翻转。`exposureEv` 对全场景乘 `2^EV`；已有 filmic/ACES fit 数学近似迁至此处，未实现完整 ACES 色彩管理。SDR sRGB-nonlinear surface 支持 RGBA/BGRA sRGB 与 UNORM：sRGB attachment 硬件编码，UNORM 用分段 sRGB transfer，二者只编码一次。其他输出色彩空间/格式明确拒绝。

环境强度 `environmentIntensity` 改变天空和 IBL 辐射值；相机 `exposureEv` 只改变显示结果。EV 必须有限且在 [-16,16]，在 acquire/fence reset 之前验证。UI 提供 [-8,8] slider 和 Tone Mapping 开关。PBR 数据 debug 1–5、shadow debug 2–3 绕过曝光和 tone mapping；辐射量 debug 6–8 保留显示变换。

ImGui 在公共输出之后绘制，其现有色彩约定保持现状。测试验证 UI 的曝光独立和调度顺序，没有宣称新增完整 UI gamma 校准。

## 生命周期与测量

resize 先完整构造新 HDR 输出和 depth/pipeline 候选，成功后交换，调用方先完成旧帧。输出 shader 构建失败时，旧 Renderer 资源和各资源域的 range/counter 保持可用，之后仍能绘制；VMA 可以缓存候选用过的 backing，最终销毁必须全部归零。该回归在现有 swapchain 上注入 Renderer 构建失败，不承诺新的 WSI swapchain 已替换后的回滚。

timestamp 从 8 增至 10 个，分别覆盖 frame、shadow、main、output、UI 边界。查询继续在既有 frame fence 完成后读取，没有增加运行时上传等待。每次 recreation 增加 9 个 pipeline build（原 8 个加 output）。

报告 schema 3 增加 `gpu_output_ms`（CSV 与 p50/p95/p99）、`exposure_ev`、`tone_mapping_enabled`、`scene_color_format`，用 `environment_intensity` 替换旧 `environment_exposure`。账本的唯一 backing/range 字段语义不变，继续排除 swapchain、ImGui backend 和驱动对象开销。

## 验证结果与界限

- Release 构建与 10 项 CTest 通过（8 CPU / 2 llvmpipe/X11 GPU），保留导入、真实 ImGui/异步加载、取消、退休、共享、失败回退和最终零资源回归。
- 四种 sRGB/UNORM RGBA/BGRA 输出，五组 EV/tone 设置，2×2 多值 palette、像素方位、分段 transfer 附近和 alpha 检查通过。HDR 回读允许一 FP16 ULP，8-bit 显示参考允许两 LSB。
- 实际 PBR pipeline 绘制 emissive 后景 `(1,2,3)` 与 alpha=0.5 前景 `(4,1,0.25)`，HDR 回读 `(2.5,1.5,1.625,1)`；EV=0/+2 都保留同一 HDR 值。显示结果跟参考一致，可区分映射后混合的错误实现。
- 实际天空不随 EV 改变 HDR，environmentIntensity=2 时原始辐射约为两倍。数据 debug、无效 EV 后恢复、nested begin 不改有效设置、私有 shader 目录构建失败后的绘制、UI 顺序与 output 查询通过。
- 400×300、640×360、窗口管理器实际初始 1262×1372 三次 resize 后绘制及恢复通过；颜色加深度 payload 为每像素 12 字节，共享场景资源保持稳定。
- 无 UI 报告烟测 24 帧、查询全部匹配；四合院 27 meshes / 27 materials，原生 1920×1080 / UI / orbit / FIFO 加载、绘制、报告与退出通过。

四合院账本：payload 295,481,120 字节，backing current/peak 302,722,140 / 534,185,720 字节，17 个物理块、137 个 range（84 buffer / 53 image）。相对 M1-C，同一场景新增 16,588,800 字节 payload 与 backing，以及一个图像、view、sampler、pool、set。软件运行的这些数值是应用所有权/分配证据，不是总显存或 4060 Ti 性能结论。

直接用本地 RenderDoc 1.45 注入当前 Vulkan 1.3 生产程序：注入成功，随后程序设备选择失败并断开，未产生新 capture。未降低生产 API，也未进行物理 GPU 排查。4060 Ti 返回后的新抓帧、validation、输出通道成本、参考画质与运动稳定性仍待验收。

本增量未补齐 `KHR_materials_specular`、typed mip、镜面抗锯齿、完整 GGX IBL/BRDF LUT；它们仍在 M3。FP16 量化/范围是场景缓冲合同，测试不能证明任意资产亮度和最终 PBR 画质均正确。

源指纹：`7a2023a5afb1b535d27e8c2341338f5dc9ad34fd6d2d0f0d68db67ae58678be8`。执行日志、软件报告、备份与本轮 delta patch 在聊天工作区 `outputs`，记录见 `M2-A线性HDR执行记录.md`。
