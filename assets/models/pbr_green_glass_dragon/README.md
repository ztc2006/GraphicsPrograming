# 绿色玻璃龙：透射与体积吸收参考

来源为 Khronos glTF Sample Assets 的 `DragonAttenuation`，固定提交
`5bad5aaa0bbb5d0f9cdc934e626f27d0df1e79b8`。原版是黄色/橙色体积吸收玻璃龙；
项目派生的绿色版本只将活动龙材质的 `attenuationColor` 改为 `(0.12, 0.85, 0.25)`，
保留白色 Base Color、100% transmission、零 roughness、原始厚度贴图、
`thicknessFactor=2.27`、`attenuationDistance=0.155`、节点坐标和缩放。
这是绿色玻璃测试，不是玉石或 SSS 测试。玻璃保持默认 IOR 1.5、OPAQUE/alpha=1；
透射不能用降低 alpha 替代。

2 个 mesh、3 个材质，总计 134,995 个三角形；有棋盘格布景和龙的厚度贴图。
上游原始文件保持在 `source/`；生成版本会将相对资源 URI 指向 `source/`。
下载与 SHA-256 清单在 `tools/pbr_test_assets.json`，派生方式在恢复脚本中。

```bash
python3 tools/fetch_pbr_test_assets.py
./run.sh --no-build assets/models/pbr_green_glass_dragon/green_glass_dragon.gltf
```

可从 Scene 面板选择以上路径，也可拖入窗口。

当前渲染器尚不支持 `KHR_materials_transmission`、`KHR_materials_volume` 和
`KHR_materials_variants`，Scene 面板会报告 optional extension/core fallback。
因此当前加载绿色玻璃版本会得到白色不透明表面，不能据此认定已支持绿色玻璃。

两个辅助入口：

```bash
# 原版橙色玻璃：在当前渲染器同样缺少体积吸收
./run.sh --no-build assets/models/pbr_green_glass_dragon/source/DragonAttenuation.gltf
# 只检验绿色表面颜色；明确不是玻璃
./run.sh --no-build assets/models/pbr_green_glass_dragon/green_surface_control.gltf
```

后续玻璃验收需检查：棋盘格通过龙身可见并折射、厚躯干比薄爪吸收更多、
非均匀/统一缩放对厚度的影响、粗糙透射、屏幕外背景与多层玻璃的近似边界。
玻璃折射与体积吸收依据 RTR4 第 9/14 章与 Khronos 扩展规范；本次没有实现新着色通道。

来源及署名：

- [固定上游](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/5bad5aaa0bbb5d0f9cdc934e626f27d0df1e79b8/Models/DragonAttenuation)
- 原龙扫描：© 1996 Stanford University Computer Graphics Laboratory。
- 转换/清理：© 2017 Morgan McGuire，Computer Graphics Archive。
- 棋盘格布景：Adobe，CC0；原始 README 保留了分项说明。
- 龙遵循 Stanford Graphics Library 条件；元文档 CC BY 4.0。见 `LICENSE.md` 和 `upstream/README.md`。
- 项目修改：2026-10-07，绿色吸收参数和明确标注的 opaque surface control；未修改原始几何。

[KHR_materials_volume](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_volume)
规定厚度纹理取 G 通道、厚度在 mesh 空间、吸收距离在世界空间。

2026-10-07：RTX 4060 Ti 实际加载/抓帧/退出检查通过；材质扩展仍不受支持。
详见 [实际对照记录](../../../docs/Material_Extension_Asset_Check.md)。
