# M3-A2：mask 覆盖率与切线空间决定

2026-10-04。实现前使用 real-time-rendering-advisor 的 texturing/PBR、transforms 与 decision playbook。约束沿用 Linux、原生 1080p/60 FPS、静态 glTF/GLB/OBJ、运动稳定性、可移动相机/物体/灯光和可编辑材质；4060 Ti 现场验收继续延后。

## 推荐与边界

采用 CPU 逐级 alpha-test coverage scaling，单一 opacity source 时生成 cutoff/material alpha factor 对应的 mask 链。RGBA baseColor 取 A；OBJ 独立 opacity map 明确取线性 R，不能取 max(RGBA)，否则 stb 补出的 A=1 会使灰度遮罩失效。与 BLEND 的 straight-alpha 平均分开，opaque/blend 使用原 A1 策略。

| 候选 | 质量/代价 | 决定 |
| --- | --- | --- |
| LOD 0 | 保留原 mask 语义，但远处会闪烁 | 作为安全回退 |
| CPU coverage scale | 不加渲染 pass；按已量化 RGBA8 的实际阈值计数；固定 cutoff/factor 的近似 | 单一来源采用 |
| GPU cutoff/CDF 校正 | 可适应更多动态参数，需要额外 LUT/访问和生成合同 | 首轮不采用 |
| Alpha-to-coverage/随机或时间覆盖率 | 可配合 MSAA/TAA，涉及采样/历史合同 | 后续 AA 阶段评估 |

用 256-bin histogram 比较可达到的离散 coverage，选择最接近 LOD 0 的统一 scale，等价误差时尽量接近 scale=1。MASK 保留 >= cutoff 的语义。不保证任意图案都在 2% 内：同值 ties、小 mip（尤其 1×1）、bilinear/trilinear/aniso 的非中心采样、阴影和相机不同 footprint 都会产生误差。代表性非退化图案在 texel 中心要求误差 <= max(2%,1/N)，ties/最后层专门记录可达到的边界。

两张 opacity 图相乘、非单位顶点 alpha、材质 cutoff/alpha 因子或 alpha mode 改变时，使用预先绑定的 base-level view 与 non-mip sampler。它与全链共享 image/storage，无新图像复制、无 mid-flight descriptor 更新、无需等待；参数恢复到烘焙值可重新启用。此回退保留语义，仍可能闪烁，不能宣称动态覆盖率/运动稳定性全部完成。有效阈值 cutoff/factor、通道、算法版本全部进入生成链的缓存身份；相同比值的材质可以共享相同链，原 cutoff/factor 仍各自保存以验证运行时编辑。parallax 与覆盖率组合先回退，避免声称两个 footprint 一致。

## 顶点与 normal/tangent

增加独立 vertex alpha，glTF VEC3 默认 1，VEC4 保留并校验 [0,1]；主场景与 shadow 使用相同 alpha 乘法。按 draw 标记顶点 alpha 是否全为 1。保持 RGB 格式和既有材质 push 常量大小。

生成切线采用官方 MikkTSpace，固定上游提交与许可，构建不下载。按 face-corner 回调和 original-index+tangent/sign 去重，在 mirrored-UV/handedness 接缝拆顶点，不能把不兼容切线平均到同一个顶点。保留已验证的 authored tangent；normal texture 的实际独立/变换 UV 仍决定生成 basis。几何零面积/退化 UV 明确使用有限的正交 fallback。

世界 normal 使用 inverse transpose，tangent 用线性 model transform，handedness 乘 determinant sign；片元插值后正交化/归一化，fallback 随 N 选择，避免 N 平行 X 时基底坍塌。双面表面先建立 front basis，再按正反面反转完整 T/B/N，与 normal map 的 +Y 约定保持一致。normalScale 作用于 tangent normal XY，再安全归一化。非均匀/负缩放、背面、normalScale=0 和 mirrored UV 必须以真实 shader 像素验收。

## 数据和验证

沿用 typed CPU chain→单 staging/多 region→全链 barrier/单队列上传。额外 base views/samplers 强绑定随候选、上传、场景与 preview 一起退休；弱缓存不保活。材质增加两个 sampled bindings，push 常量仍 112 bytes；顶点格式多一个 float，账本记录真实 payload 增量。

CPU 量化 coverage、cache 参数差异、vertex alpha 导入、Mikk mirrored seam/degenerate/UV、finite basis；ASan/UBSan。软件 Vulkan 主/shadow discard 和 depth 计数、LOD/参数修改/vertex alpha/OBJ R、实际 normal debug 像素（单位长度误差 <= .015、方向通道误差 <= .02）。既有全部异步、取消、重复共享、退休、图/HDR、一/两帧/present/resize/归零回归保持。四合院报告记录内存与加载观察，不以软件速度宣称 60 FPS。

## 依据

RTR4 第 4 章变换、第 5–6 章透明/纹理、第 9 章材质为稳定依据。2026-10-04 核对 [Castaño 原始 coverage 方法](https://www.ludicon.com/castano/blog/articles/computing-alpha-mipmaps/)：统一缩放和离散误差；本项目改用量化 histogram 搜索。核对 [Khronos glTF 规范](https://github.com/KhronosGroup/glTF/blob/main/specification/2.0/Specification.adoc)：COLOR_0、normalScale、mask 和 tangent/MikkTSpace；以及 [MikkTSpace 上游](https://github.com/mmikk/MikkTSpace)。shader 基底与 [Khronos sample renderer](https://github.com/KhronosGroup/glTF-Sample-Renderer/blob/main/source/Renderer/shaders/material_info.glsl) 比较，不把该渲染器整体接口搬入项目。未引入新的后 RTR4 GI/AA 算法。

实现前补充：检查 draw 选择发现负 determinant 的单面物体仍用原 cull
方向，可能整面消失。为 opaque/blend/shadow 各预建一个相反 cull variant
（共 12 次 pipeline builds，原 9），按 draw determinant 选择；双面保留
无剔除 variant。片元 front/back 判断同样按 determinant 修正，避免把镜像
变换的原正面当背面。无需额外 dynamic-state feature。测试同时覆盖
单面镜像物体可见和双面原背面的完整 TBN 翻转。
