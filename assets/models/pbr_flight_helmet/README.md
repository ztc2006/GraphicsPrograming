# Flight Helmet：纹理 PBR 参考

使用 Khronos glTF Sample Assets 的原始 core metallic-roughness 版本，
固定提交 `5bad5aaa0bbb5d0f9cdc934e626f27d0df1e79b8`。
这是 2023-11 加入 transmission 之前的上游资产，未自行删除扩展。
6 个 mesh、6 个材质、94,722 个三角形，15 张 1024/2048 PNG，
每个材质具有 base color、normal 与 occlusion/roughness/metallic 纹理引用。
资产和来源记录共 48,393,766 字节。

[上游固定版本](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/5bad5aaa0bbb5d0f9cdc934e626f27d0df1e79b8/Models/FlightHelmet)
保留在 [upstream/README.md](upstream/README.md)。模型/纹理为 CC0，许可元文档为
CC BY 4.0，完整分类见 [LICENSE.md](LICENSE.md)。保留上游署名：Gary Hsu，Maya 转换；
© 2018 Public。

在项目根目录运行：

```bash
python3 tools/fetch_pbr_test_assets.py
./run.sh --no-build assets/models/pbr_flight_helmet/source/FlightHelmet.gltf
```

也可从 Scene 面板加载或拖入窗口。用于检查皮革、橡胶、金属、木材的颜色空间、
ORM 通道、切线空间、法线 mip、镜面抗锯齿和 IBL；观察粗糙度/金属/法线 debug
后再看固定曝光下的最终材质。镜片遵循该旧版原始 core 材质，不用于验证真实透射。
这是单物体材质参考；完整场景覆盖由 [Country Kitchen](../pbr_kitchen/README.md) 提供。

2026-10-05：实际导入、纹理解码/上传、呈现、GPU query、资源账本和退出的
llvmpipe 烟测通过；硬件画质与性能验收仍待 4060 Ti。PNG/bin/glTF payload
不纳入 Git 二进制历史，通过固定版本/SHA-256 manifest 和脚本复现。
