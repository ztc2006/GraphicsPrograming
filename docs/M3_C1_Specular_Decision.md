# M3-C1：必要材质扩展的实现决策

2026-10-04。先记录决策，再修改图形代码。应用 real-time-rendering-advisor：RTR4 第 9 章微表面/菲涅耳，第 10 章环境光；本地检索第 270–271 PDF 页确认 Schlick 模型。Linux / Vulkan 1.3，原生 1080p / 60 FPS，4060 Ti 硬件验收延后；本轮使用 llvmpipe/X11 `sw,noshm`。

## 选择与边界

实现 KHR_materials_specular 的强度、颜色、两张贴图及独立 UV/sampler/texture transform。四合院材料 7 Leaves.001 和 8 Bark.001 使用颜色贴图，颜色因子分别约 [1.1351,1.1593,1]、[1.1176,1,1]。cgltf 内部有解析结果，现有 Material 适配器丢弃它们；旧进度“CPU import retains”需要更正。

继续使用单次散射 GGX、现有 A/B LUT 和 SH；本轮不改变 BRDF 几何函数、缓存 bake 或 HDR 输出。与只乘 F0 相比，同时缩放 F90 才能让强度为零时彻底关闭介电高光；纯金属不受扩展影响。与复制整个 Sample Renderer 的多重散射 IBL 相比，保留已验证 LUT 可控制本轮变量，但保留已有 roughness Fresnel 漫反射 IBL 近似边界。未来更换模型必须同步 LUT/数值参考。

## 数据与计算

强度纹理使用线性 A，颜色纹理使用 sRGB 解码后的 RGB；都乘对应因子。颜色因子非负、有限且允许大于 1；强度因子必须 [0,1]。缺失贴图使用已有中性白色绑定，不上传新 fallback。强度不影响 opacity、alpha mask 或 coverage mip。与 unlit/specGlossiness 同材料冲突直接诊断；其它未支持扩展仍按 required 拒绝、optional 明示核心回退。

介电 F0 = min(0.04 × color, 1) × strength；F90 = strength。金属使用 baseColor / F90=1。直接光先计算介电和金属响应，再按 metallic 混合；介电漫反射乘 1-max(Fresnel RGB)，避免反色。IBL 镜面使用混合后的 F0×A+F90×B；漫反射保持 roughness Fresnel 近似，对未乘强度的介电 F0 计算后乘强度，再取最大分量并乘 (1-metallic)。不得额外乘第二次强度或 Fresnel。颜色因子钳制仅发生在 F0 反射率乘积，不能先钳制作者颜色。

API 无关资源流：导入器解析并验证→每顶点两组已变换 UV→不可变场景材质常量和两种纹理解释→候选批量上传→完成后原子提交→按最后完成帧退役。沿用图形队列上传/fragment-read 屏障；无新 pass，无运行期描述符或 UBO 原地更新。

Vulkan 落地：Material UBO 32→48 字节（新增 RGB color + A strength），std140 vec4/float32；Vertex 96→112 字节，追加两组 UV，位置 10/11，保持 alpha 9 和旧字段偏移。材质增加 binding 10/11，共 11 个 combined samplers；frame 5 个，总计 16。创建布局前检查实际设备 per-stage/set sampler、sampled-image 和资源限额，低于需求报清晰错误。Frame UBO 448 和 push 112 字节不变。更紧凑的可选布局/纹理数组会增加 pipeline/cache 复杂度，后续有额外扩展压力再设计。

## 验证

扩展 required 可加载，默认、中性、因子、嵌入/文件引用、两组 UV/变换/采样器、缺失 UV/图像/非法因子/互斥扩展/其它未知扩展均需 CPU 断言；四合院真实值与引用需回归。生产 GLSL 的 HDR 像素与独立 double CPU 参考比较，覆盖 strength=0/.35/1、RGB 有色与超范围、metallic=0/.4/1、NoV=.1/.5/1、直接/IBL/漫反射/EV 与贴图 alpha/sRGB/独立 UV。HDR 允许 max(.008, .003×expected) 误差；金属不变、零强度无高光、漫反射不产生反色。所有四种 one/two frame + auto/fence/legacy 事务继续通过，资源退役最终归零。再做 UI 四合院 one/two 帧报告、SPIR-V 和 importer ASan/UBSan；软件帧数只作为功能证据，不是 60 FPS 承诺。

## 证据

- [Khronos 扩展规范](https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_specular/README.md)：已批准规范，2026-10-04 核对纹理编码、F0/F90、scalar diffuse 及超单位颜色因子。
- [官方 Sample Renderer material_info](https://github.com/KhronosGroup/glTF-Sample-Renderer/blob/main/source/Renderer/shaders/material_info.glsl)、[pbr](https://github.com/KhronosGroup/glTF-Sample-Renderer/blob/main/source/Renderer/shaders/pbr.frag)、[IBL](https://github.com/KhronosGroup/glTF-Sample-Renderer/blob/main/source/Renderer/shaders/ibl.glsl)：2026-10-04 核对实际应用入口；其 IBL 多散射与本项目 A/B 单散射模型不同，不作为逐像素等价参考；RGB diffuse 采用扩展规范定义。
- agent-reach 的 Jina Reader 成功读取官方规范；官方 raw 页面用于交叉核对。硬件 RenderDoc/运动画质仍待用户切换 4060 Ti。
