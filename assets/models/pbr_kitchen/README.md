# Country Kitchen：完整场景与材质剖面

原作 **Jay-Artist / Country Kitchen**，由 Benedikt Bitterli 收录，Erfan Momeni
转换为 glTF。资产采用 [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/)，
请保留作者署名、许可链接及修改说明。原始声明在 [LICENSE.txt](LICENSE.txt)。
转换器作者的说明与转换记录采用 MIT，见 [upstream/LICENSE](upstream/LICENSE)。

- [固定来源版本](https://github.com/ErfanMo77/gltf-research-scenes/tree/50cbe72d462cfbbf11244d0df6a5501a74050d34/scenes/kitchen)
- [原作](https://blendswap.com/blend/5156)
- [场景收录来源](https://benedikt-bitterli.me/resources/)
- [转换记录](upstream/conversion.yaml)

下载的是未经修改的 **core glTF**，299 个 mesh、90 个材质、1,443,517 个三角形、
20 张嵌入 PNG，资产及来源记录共 43,273,359 字节。它适合观察金属与电介质、
不同粗糙度、透明排序、遮挡关系、加载规模和未来的阴影/反射。
它没有法线贴图或 metallic-roughness 贴图；纹理通道/切线/法线滤波由
[Flight Helmet](../pbr_flight_helmet/README.md) 与解析参考场景补充。

在项目根目录运行：

```bash
python3 tools/fetch_pbr_test_assets.py
./run.sh --no-build assets/models/pbr_kitchen/kitchen_cutaway.gltf
./run.sh --no-build assets/models/pbr_kitchen/source/kitchen_core.gltf
```

也可在 Scene 面板进入本目录，选择 glTF 文件再 Load，或者拖入窗口。

`kitchen_cutaway.gltf` 是本项目生成的明确修改版本：仅从活动场景移除
`126_Ceiling`、`127_Walls`、`263_Walls`、`264_Walls`，重指向同一份二进制。
保留 295 个活动 mesh、1,442,894 个三角形，材质、纹理、物体变换不改。
它方便当前按 bounds 放置的相机直接观察材质。剖面改变了遮挡，不能作为
封闭室内的光照、AO、GI 或反射参考。完整版本仍保留全部结构；按右键配合
WASD/Space/Ctrl 飞入室内。当前查看器不自动采用资产自带相机。

core 转换中的玻璃是 alpha blend 近似；清漆、透射、IOR、光谱金属和 bump/displacement
不能视为原 PBRT 的精确还原。四个原始面光源在资产中是 emissive mesh，当前引擎
不会因这些网格而照亮附近物体。资产没有 `KHR_lights_punctual`，使用项目默认
太阳光/IBL，也可通过 Lighting 添加局部灯。不要用上游路径追踪截图作为当前
core 材质与默认光照的逐像素基准。

2026-10-05：原始场景的实际导入/上传/呈现/报告/退出已通过 llvmpipe 烟测。
剖面另以 run.sh、UI 和两帧配置检查。虽显式请求 Vulkan validation，当前环境
缺少 `VK_LAYER_KHRONOS_validation`，实际未启用，不能将零错误计数视为验证层通过。
4060 Ti 的画质、运动稳定性、GPU 帧预算及 RenderDoc 验收继续等待换机通知。
下载 payload 和剖面由脚本重建，不加入 Git 二进制历史；固定版本、大小、SHA-256
保存在 `tools/pbr_test_assets.json`。
