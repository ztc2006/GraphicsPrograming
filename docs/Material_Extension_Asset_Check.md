# 各向异性与绿色玻璃龙资产检查

日期：2026-10-07。用户确认想要绿色玻璃龙，而非玉石/SSS 龙。
本轮加入资产和复现入口，使用已有渲染路径实际抓帧，没有实现新的材质着色功能。

## 资产

| 资产 | 用途 | 版本与修改 |
| --- | --- | --- |
| AnisotropyBarnLamp | 拉丝铜方向纹理与各向异性高光 | Khronos 原版 glTF/PNG，保留 5 个材质扩展 |
| DragonAttenuation 原版 | 彩色体积吸收、厚度与折射 | Khronos 原版橙色玻璃龙与棋盘格布景 |
| green_glass_dragon | 绿色玻璃 | 仅活动材质吸收色改为 `(0.12,0.85,0.25)`；白色表面与原厚度保留 |
| green_surface_control | 表面颜色对照 | 移除材质扩展，将不透明 Base Color 改绿；明确不是玻璃 |
| anisotropy_off_control | 同场景各向异性对照 | 仅将 `anisotropyStrength` 从 0.75 改为 0 |

上游固定为 `5bad5aaa0bbb5d0f9cdc934e626f27d0df1e79b8`，新增 16 个文件、
17,347,647 字节。全部 41 个已登记源文件共 109,014,772 字节，SHA-256 校验通过。
新增几何/纹理文件还核对了固定目录返回的 Git blob SHA-1。
原始资产不修改；派生 glTF 可通过 `tools/fetch_pbr_test_assets.py --verify-only` 重新生成。

龙扫描来自 Stanford，转换/清理来自 Morgan McGuire；棋盘格布景来自 Adobe。
铜灯来自 Wayfair / Eric Chadwick，CC BY 4.0。保留原始许可与元文档，
分项限制见各资产 README/许可证，未把龙统一重标为 CC0。

## 实际结果

在 NVIDIA GeForce RTX 4060 Ti、原生 1920×1080、单帧槽、静态相机、TAA 开启、
asset 灯光设置下，5 个入口都完成了真实上传、绘制、RenderDoc 抓帧与退出。
每个入口 5 个测量帧，所有帧 GPU queries 有效且回收；呈现待完成 fence 为 0，
prepared/retired/staging 资源账本清空。捕获序列包含 34 个龙场景动作/39 个铜灯场景动作。
这些短且注入 RenderDoc 的运行只验证兼容与画面，不作为 60 FPS 性能验收。

| 配对 | 最终截图逐像素比较 |
| --- | --- |
| 原版橙色体积玻璃 vs 绿色体积玻璃 | 2,073,600 像素全部相同，RGB 最大差 0 |
| 各向异性 0.75 vs 0 | 2,073,600 像素全部相同，RGB 最大差 0 |
| 绿色体积玻璃 vs 绿色 opaque 表面 | 36,683 像素不同，RGB 8-bit 最大差 130 |

当前 `Material`/glTF 适配器/PBR shader 未承接 transmission、volume、anisotropy。
加载器对这些 optional 扩展报告 core fallback。因此白色玻璃表面按不透明材质绘制，
吸收色没有生效；把 Base Color 改绿能变绿，但这只是绿色不透明表面。
铜灯纹理中的拉丝细节正常出现，各向异性方向高光却没有参与运算。

这不是测试已通过玻璃/各向异性画质的结论。玻璃缺的是材质透射、背景可见性/折射与
沿厚度的体积吸收；各向异性缺的是稳定切线方向的 BRDF 及相应的环境反射近似。
普通 alpha blending 不等同于玻璃，纹理 anisotropic filtering 不等同于各向异性 BRDF。

## 后续材质验收

当前路线继续保留 M6-C 轨迹验收/WSI 排查 → M7 GTAO 等既定工作，
没有因为下载资产自动插入整个玻璃/各向异性实现里程碑。资产现在可用于后续扩展的验收。

玻璃实现要先明确背景 HDR/深度来源、屏幕空间和屏幕外回退、粗糙透射、厚度/缩放、
吸收距离单位、多层排序与资源依赖。测量时同时查看背景棋盘格折射、躯干/薄爪颜色差别，
并控制曝光。各向异性实现要检查旋转方向、纹理 RG/B、强度为零的退化、镜像切线和 IBL。

RTR4 第 9/14 章提供表面散射、透射/体积处理的基础；
[Khronos volume 规范](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_volume)
规定厚度纹理取 G 通道、厚度在 mesh 空间、吸收距离在世界空间，且不规定体积散射。
[anisotropy 规范](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_anisotropy)
定义切线方向与强度。这里不把玻璃透射测试当作玉石 SSS 测试。

## 启动与证据

```bash
python3 tools/fetch_pbr_test_assets.py --verify-only
./run.sh --no-build assets/models/pbr_green_glass_dragon/green_glass_dragon.gltf
./run.sh --no-build assets/models/pbr_anisotropy_barn_lamp/source/AnisotropyBarnLamp.gltf
```

可在 Scene 面板选择或拖入上述 glTF。各资产 README 有两个对照入口。

本次聊天 `outputs/material-extension-assets/` 保存 `.rdc`、重放 PNG、逐像素比较 JSON、
5 个 viewer report/CSV 与运行脚本；主结果是 `rtx-asset-checks.json`、
`image-comparisons.json`。场景源文件和派生规则也已加入项目资产清单，
PNG/bin/glTF 下载负载继续排除在 Git 历史之外。
