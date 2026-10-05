# M3-A2：遮罩覆盖率与切线空间实施合同

2026-10-04。图形代码前按 real-time-rendering-advisor 核对纹理、变换和 PBR；替代方案与主来源见 [决定](M3_A2_Material_Decision.md)。本增量的软件实现和回归已完成，4060 Ti 现场画质/性能验收继续延后。

## 遮罩与编辑

`AlphaCoverage` 使用线性透明度、256-bin histogram 和 RGBA8 的实际 `>= cutoff` 判定。在每个面积平均 mip 后选择使通过比例最接近 LOD 0 的统一缩放；同样误差时选最接近 1 的缩放。LOD 0 不改，RGB 与其他数据通道不参与透明度缩放。覆盖率链身份包含编码内容、颜色空间、策略、版本 2、有效阈值 `cutoff / material alpha` 和 A/R 通道。相同比值可共享；原 cutoff/factor 各自保存以判断编辑是否仍匹配。

单一来源的 MASK 使用该链：glTF baseColor 取 A；OBJ 独立 opacity map 取线性 R。修复旧 `max(RGBA)` 使灰度图补出的 A=1 覆盖遮罩的问题。OBJ 自定义 `-imfchan` 语义没有在本次扩展，R 是当前兼容合同。

每个材质预绑定 base albedo/opacity（bindings 8/9）。LOD 0 view 与完整 view 共享 image/storage，弱缓存按 image 复用 base view；non-mip sampler 保留 min/mag/wrap。不会复制另一份图像，也不会在飞行帧修改 descriptor。两来源相乘、非单位 vertex alpha、active parallax，或更改 cutoff/factor/mode 时回退原始 LOD 0；恢复参数重新启用已绑定的覆盖率链。`alphaParams.z` 为 ordinary/matching/edited 状态 0/1/2，w 为本次 draw 的单位 vertex alpha 与无 active parallax 标志；push 常量仍为 112 bytes。MASK→BLEND 必须取原 alpha；OPAQUE 忽略 alpha，普通 BLEND 保持 straight-alpha 合成。

glTF COLOR_0 VEC4 alpha 现在进入独立 float vertex attribute 9；VEC3 默认 1，RGBA 超出 [0,1] 明确拒绝。主通道和阴影通道使用同一透明度乘法。颜色/透明度双来源、参数编辑和不同 camera/light footprints 的所有过滤组合并不因此获得严格的面积覆盖率保证。

## 切线与变换

使用固定 MikkTSpace 提交 `3e895b49d05ea07e4c2133156cfa94369e19e409`，源码与许可保留在 `deps/mikktspace`，构建不联网。生成器按三角形 corner 接收 tangent/sign，按原顶点与切线去重；不兼容的镜像 UV 接缝拆顶点，兼容顶点保留原顺序。已提供的 authored tangent 仍保留，normal texture 独立/变换 UV 决定需要重新生成的 basis。退化 UV 和零面积几何保留有限正交 fallback；回调也投影掉与 N 平行的退化切线。

normal 使用 inverse transpose，tangent 使用线性变换，handedness 乘 determinant sign；片元正交化和自适应 fallback 避免 N 平行 X 的基底坍塌。双面原背面同时反转 T/B/N，normalScale 只作用于 tangent normal XY，再安全归一化。负 determinant 修正材质正反面判断，并选择相反剔除的单面 opaque/blend/shadow variant。新增三个预建 variant，总数从 9 变为 12；编辑材质与重载模型不会重建这些管线，不要求额外 dynamic-state feature。

顶点格式增加一个 float；切线接缝可能增加顶点数量。normal mip 的归一化仍没有保存法线方差；镜面抗锯齿、完整 IBL、必要扩展 shader 语义留在后续 M3。

## 实际验证

Release/Ninja：**19/19 CTests**，12 CPU + 7 llvmpipe/X11 Vulkan，无跳过。软件呈现环境显式使用 `VK_DRIVER_FILES=lvp_icd.json`、取消 `WAYLAND_DISPLAY`、`MESA_VK_WSI_DEBUG=sw,noshm`；正常启动不强制这些变量。CPU mip/Mikk（含上游 C 文件）ASan/UBSan 与四个主/阴影 shader 的 SPIR-V validation 通过。

- CPU：普通 mip 会把代表性图案的 LOD 1 通过比例降为 0/16，coverage 得到 8/16；多阈值、R/A、LOD 0/RGB 保留、ties/1×1 离散极限通过。不能对任意图案承诺 2%：中心采样的代表性误差界为 max(2%,1/N)，均值 ties 和插值另有边界。
- 导入/Mikk：VEC3/VEC4 alpha、非法颜色拒绝、authored tangent、兼容与镜像 seam、独立 normal UV、退化 UV/几何和 OBJ UV 默认值通过。
- 真实 GPU cache/probe：阈值/通道分开，相同输入共享；base view 不增加 image/payload/copy；实际 LOD 1 sampled coverage=8/16。
- 真实生产 main/shadow shader：R 遮罩和 vertex alpha 的 discard、相机与光源各自 8/16 depth 计数、cutoff/factor/vertex/POM 回退与恢复、MASK→BLEND 原 alpha、负缩放单面 opaque/blend/shadow 可见均通过。
- 真实 normal-debug HDR 像素：非均匀/负缩放、单面/双面背面、Mikk 镜像 UV、normalScale=0 与平行 tangent fallback；方向误差 <.02、长度误差 <.015。
- 原事务在全部四种 GPU 配置保持 4 commits / 6 frames / 5 scene upload submissions / 7 image copies / 0 runtime upload fence waits；重复加载仍复制零张图。所有最终账本归零；一/两帧、EXT/legacy、取消/preview 退休、ImGui 加载、图/HDR/resize/查询交付保持通过。环境/管线冷启动计数为 1/12。

报告 smoke 一帧/auto、两帧/fence、两帧/legacy 有效完整行数为 19/12/15。四合院 1920×1080、UI/orbit/FIFO/require-fence，一/两帧各 4 行/queries/presents；pending=0、释放证明为 true，prepared/retired/staging=0。

| 四合院当前资源 | 一帧 | 两帧 |
| --- | ---: | ---: |
| buffers / images / image views / samplers | 84 / 53 / 73 / 6 | 85 / 53 / 73 / 6 |
| descriptor sets | 29 | 30 |
| payload bytes | 348,266,420 | 348,266,868 |
| resource suballocated bytes | 348,620,452 | 348,620,900 |
| VMA unique blocks / backing bytes | 20 / 368,831,872 | 20 / 368,831,872 |
| backing peak bytes | 662,145,528 | 662,145,528 |

对比 A1：各多 20 views、1 sampler；无额外 image。payload 各增加 8,151,300 bytes，其中 live scene geometry 4,830,776、常驻 debug geometry 32、MASK 完整 mip 3,320,492。resource suballocation 增加 8,155,736，unique backing 增加 1,738,788。以上为项目资源账本，不含 CPU 资产、driver/swapchain/ImGui 开销，不是总 VRAM/residency，不能把 backing 与 suballocation 相加。

加载观察一/两帧约 3.39/3.54 s，A1 为 2.19/2.22 s；单次软件运行，没有受控 A/B 和独立 decode/mip/Mikk/staging 计时，不能归因全部增量或据此判断稳态 GPU 帧率。若后续测量确认加载生成是瓶颈，再评估离线/缓存资源。

## 记录与后续

当前 source SHA256 `41529a3f3fe050ed081199ae6256327d088a49f1abbcc3de0d7721fbba846005` 包含固定 Mikk C/H，与 build header 和两个四合院报告一致。聊天工作区保留 `outputs/m3a2-verification.json`、完整测试日志、报告和 `m3a2-material-only.patch`；patch 从保存的 A2 前置状态生成，不混入先前本地阶段，应用后逐文件内容/执行权限复现当前切片。本增量与 M2-B 至 M3-C1 纳入同一次集成提交。

下一项 M3-B：GGX cubemap prefilter、BRDF LUT、环境烘焙缓存与 SH 校准。先参考 advisor 并写采样/精度/烘焙/缓存/上传决定，再实施。4060 Ti 画质、时序/VRAM、validation、新 RenderDoc、原生 WSI 与最小化等待继续延后；现有 legacy 释放证明和原生 MIT-SHM/DRI3 顺序故障仍开放。coverage/POM 回退不解决真实几何阴影偏移，不能宣称整体运动稳定性目标已经完成。
