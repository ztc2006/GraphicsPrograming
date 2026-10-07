# 拉丝铜灯：各向异性材质参考

下载 Khronos `AnisotropyBarnLamp` 的 PNG/glTF 原版，固定提交
`5bad5aaa0bbb5d0f9cdc934e626f27d0df1e79b8`。3 个 mesh/材质、10,203 个三角形，
包含法线、Base Color、ORM、各向异性方向/强度纹理。
铜材质使用 `anisotropyStrength=0.75`、`anisotropyRotation=0.785398` 和方向纹理，
另有 clearcoat、灯丝 emissive_strength 与灯泡 transmission/volume。

```bash
python3 tools/fetch_pbr_test_assets.py
./run.sh --no-build assets/models/pbr_anisotropy_barn_lamp/source/AnisotropyBarnLamp.gltf
# 对照仅将 anisotropyStrength 改为 0，原始纹理、几何与其他材质参数保持一致
./run.sh --no-build assets/models/pbr_anisotropy_barn_lamp/anisotropy_off_control.gltf
```

也可通过 Scene 面板或拖入窗口加载。当前只有纹理采样的 anisotropic filtering，
没有各向异性 BRDF；上述扩展会明确报告 core fallback。
因此拉丝纹理可见，但各向异性高光方向并不生效。该对照用于暴露材质能力缺失，
不能将普通 GGX 的表面细节误认为实现了各向异性反射。

后续验收要分别检查旋转角、纹理 RG 方向、B 强度、零强度退化、镜像 UV/切线、
点光与环境反射的方向变化；普通各向同性预滤波 IBL 不能直接视为完整各向异性 IBL。
依据 RTR4 第 9 章及 Khronos 扩展规范。

© 2023 Wayfair, LLC；Eric Chadwick。资产和元文档 CC BY 4.0，保留
`LICENSE.md`、`upstream/README.md` 与 metadata。
项目修改：2026-10-07，额外的零强度控制版本和相对资源 URI 重定位；原版保持完整。

[固定上游](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/5bad5aaa0bbb5d0f9cdc934e626f27d0df1e79b8/Models/AnisotropyBarnLamp)

2026-10-07：RTX 4060 Ti 实际加载/抓帧/退出检查通过；材质扩展仍不受支持。
详见 [实际对照记录](../../../docs/Material_Extension_Asset_Check.md)。
