# M1-C：导入与分配基础设施选择

日期：2026-10-03。范围：Linux、静态 glTF/GLB，保留项目资产类型、异步准备/提交、取消/退休与资源账本。硬件验收继续等待用户回到 RTX 4060 Ti。

## 选择与证据

| 候选 | 官方接口/依赖 | 本项目判断 |
|---|---|---|
| [cgltf](https://github.com/jkuhlmann/cgltf/tree/v1.15) | MIT、单头文件、C API、没有额外运行依赖；解析 glTF/GLB，提供 buffer 加载/验证与 accessor 转换 | 本轮采用 1.15，适配层掌握材质、UV、静态几何和不支持功能的策略。没有做解析速度排名。 |
| [fastgltf](https://github.com/spnda/fastgltf) | MIT 核心、C++ API、依赖 simdjson；提供 accessor 工具 | 项目 C++23 能接入。当前优先降低迁移范围；若 CPU 导入成为实测瓶颈，再用同一资产集比较。 |
| [TinyGLTF 当前主线](https://github.com/syoyo/tinygltf/tree/release) | 官方 README 已是 v3 C API，v1/v2 移入 attic；内含独立 JSON 后端及其第三方许可证，图像解码可选 | 不能按旧的 C++/nlohmann 印象评估。此次选择固定 cgltf API，避免同时引入另一套 JSON/图像处理路径。 |
| [VMA 3.3.0](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/tree/v3.3.0) | MIT；Vulkan 内存类型选择、子分配、映射/flush、统计与 backing block 管理 | 下一步采用的分配候选，本轮不迁移。需要先确定账本中 block 与 suballocation 的统计合同。 |

外部查询使用 agent-reach 的路由；本机 gh 缺失，经官方网页、Jina 和公开原始源码核对。没有安装查询工具或在构建时下载依赖。

## 已实现的导入适配层

`deps/cgltf` 固定 v1.15 / `bbeb5b0b070ddacddac6852fb72143eb68454937`，保留许可证和 `provenance.json` SHA256；M1-C 当时未修改上游文件；M4-A 的灯光属性存在性补丁见 [记录](../deps/cgltf/LOCAL_PATCHES.md)，provenance 同时保留原始和本地哈希。CMake 建独立库，依赖源码纳入测量 source fingerprint。UI/Renderer 不接收 cgltf 类型。

`gltf_loader.cpp` 替换手写 JSON、GLB、buffer/accessor 解析。RAII 释放解析结果；项目层在 accessor 解包前校验 bufferView 范围、stride、alignment、sparse 顺序和索引边界，再调用库验证。减法/除法检查避免 offset/count 溢出。

支持 sparse 属性、无基础 bufferView 的零初始化、normalized 转换、交错属性、sparse 32 位索引和 authored tangent/handedness。1.15 的 sparse 属性解包继承基础 stride，而规范要求 sparse values 紧密排列：项目分别解包基础和覆盖值再 scatter；索引 helper 不处理 sparse，项目使用精确整数读取，不经 float 丢失精度。

保留逐贴图 sampler、独立 TEXCOORD（包括四合院的 set 5）、内嵌/data URI/外部图像、核心 PBR 因子、alpha mode、双面语义。`KHR_texture_transform` 按 offset + rotation × scale × UV 烘焙到各贴图独立 UV，支持 texCoord override；法线 UV 改变时重建对应 tangent。没有 NORMAL 时按三角形生成 flat normal。无 material 的 primitive 不再误用作者的 material 0。

节点以精确矩阵传递，不经 Euler 分解；只实例化选中场景，迭代遍历避免递归栈增长。库检测树循环，适配层拒绝重复引用。原 glTF/GLB 公共函数签名与 OBJ 路径保留。

## 扩展与功能边界

已实现的 required 扩展仅 `KHR_texture_transform`、`KHR_mesh_quantization`；其他 required 扩展直接错误，旧场景保持可用。未知/尚未实现的 optional 扩展使用核心 fallback，并随候选场景提交警告，显示于 Scene 面板和日志；失败、取消不替换旧场景警告。

库能解析扩展不等于渲染器实现它。四合院的 `KHR_materials_specular` 仍待 M3 的项目材质字段与 shader 接入，不宣称材质完整还原。Draco/meshopt、skinning、GPU instancing、morph targets 均未进入这条静态路径；需要解码/变形的内容明确拒绝。当前 vec3 vertex color 保留 RGB，但拒绝非 1 的 vertex alpha，RGBA 格式迁移列入 M3。纹理仍是单 mip，地面/屋瓦缩小闪烁与运动画质仍未验收。

## 下一步：分配适配层

先通过 Device 的 buffer/staging 所有权接口接入 VMA，再迁移 Texture、depth/shadow 图像。不要在同一增量改变上传队列、材质、Render Graph。

- 每个资源 lease 记录 payload 和它的 suballocation；真正 Vulkan backing block 另记一次。不能把共享 block 的总大小重复算到每个资源，也不能把 suballocation 当成实际总物理分配。
- 利用 VMA block 回调/统计与对象统计交叉检查。block 可以跨 prepared/live/retired/shared 域，域归属表达对象/子分配；独立的 allocator backing 总量表达物理分配，driver heap/budget 仍独立。
- paired RAII owner 管理 buffer/image 与 VmaAllocation 的释放，避免 Vulkan-Hpp 和 VMA 双重 destroy；allocator 在所有 worker、上传、帧退休及资源销毁之后释放。
- staging 映射/非 coherent flush 范围通过适配接口处理；默认线程保护不能代替现有 queue 外部同步。
- 保留实际像素回读、共享稳定、失败/取消保活、退休、shutdown 归零和峰值的 GPU 回归。子分配不降低场景上传有效字节；大 staging 峰值需要后续独立的上传预算/节流设计。

验证结果与日志见本轮《M1-C 导入适配执行记录》。这一步结束后才进入 M2 的线性 HDR 与最小 Render Graph。
