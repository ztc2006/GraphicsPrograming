# M1-C：VMA 图像分配适配

2026-10-03。承接 [buffer/staging 适配](M1_C_VMA_Buffer_Adapter.md)，VMA 版本、离线依赖和 MIT/provenance 保持一致。

## 所有权与边界

公共适配层为 `GpuAllocator`，返回项目 `GpuBuffer` / `GpuImage`；VMA 类型只留在 `gpu_allocator.cpp`。两者复用私有分配所有权，先销毁 Vulkan handle，再释放 VmaAllocation，最后注销资源 lease。图像使用 vmaAllocateMemoryForImage / vmaBindImageMemory，保留 Vulkan-Hpp 的 handle 析构职责，避免重复 destroy。

Texture/HDR 的共享 storage 持有 GpuImage，view 持有 storage，sampler 独立持有。缓存身份、上传批次、发布票据、取消保活和退休 fence 不变。Depth/shadow 持有 GpuImage；move assignment 先释放旧视图/sampler，再替换图像所有权，默认析构同样保证依赖对象先销毁。Swapchain 图像仍由 WSI 管理，不进入分配器账本。

Resident allocator 同时管理 buffer 和 image，保留默认 buffer-image granularity 处理，不设置 IGNORE_BUFFER_IMAGE_GRANULARITY。Upload allocator 只管理独立生命周期的 staging buffer；图像不会延长上传块寿命。Device 工厂只保存 weak 引用，最后一个相应资源销毁时，allocator 及缓存块一起释放。

适配器只支持单分配完整绑定的普通图像，明确拒绝 sparse/disjoint、零尺寸和缺失 format/usage/payload。payload 由创建方提供，当前 RGBA8、RGBA32F、D32F 各自按有效数据字节计算；不把 Vulkan 内存需求误当 payload。mip/格式能力、图像布局、上传、视图和同步仍由调用方管理，本轮不扩展 mip 链或材质语义。

## 账本合同（报告 schema 2）

唯一物理 VkDeviceMemory 个数/字节全部在 `allocator_blocks` 域。所有资源域的 backing 字节为零，buffer/image 各自记录 payload、对象和 suballocation 范围。资源 suballocation 不加进 backing；dedicated VMA 分配也以 block callback 计物理 backing 一次。

UI 与报告中的原 buffer range 说明改为 resource range。字段保持 schema 2 已有泛化命名，`suballocations == buffers + images`；view/sampler 不占独立 allocation。driver heap、ImGui backend、swapchain storage、驱动开销和 CPU 数据仍独立或排除。

统计在静止检查点与 VMA 权威 blockCount/blockBytes/allocationCount/allocationBytes 交叉比较。失败/取消必须恢复资源和 range；空闲 backing 可保留到 allocator 生命周期结束，因此不要求每次失败立即恢复缓存块字节。最终 Renderer/Application/最后资源销毁要求全部归零。

## 验证

Release 构建成功，10 项 CTest（8 CPU、2 llvmpipe/X11 GPU）全部通过。最终 GPU 用例再次通过，新增用例覆盖混合 buffer/optimal image granularity 和对齐、color/depth、移动替换、创建 view 后故障回退、最后 image 独自保活 allocator、RGBA32F 大于 1 的精确回读；resize 用 400×300 → 640×360 → 真实初始 framebuffer，每次渲染并检查 depth range、共享资源和场景资源。最终独立运行的初始尺寸为 1262×1372；桌面窗口管理器可修改创建尺寸，测试不能假定初始一定是 320×240。

Application 的真实 ImGui 预览、最新请求失败/警告回退、上传取消、准备中退出及最终归零保持通过。事务用例仍为 4 commits / 6 frames / 5 uploads / 6 image copies / 0 runtime upload waits，随后新增 3 次 resize/绘制。报告烟测 34 帧与 GPU query 一一对应；独立四合院 27 meshes/materials、1920×1080、UI/orbit、1 frame/query、正常加载/绘制/退出。源码 fingerprint：`eba5c8c952e6ec7a367cfc23ea0f5033e7e91645e028899f17db7975457bc418`。

## 软件内存代价

四合院相同 payload 278892320 字节；物理 backing 与有效范围分开记录：

| 软件配置 | 常驻 backing MiB | backing 峰值 MiB | 常驻 VkDeviceMemory |
|---|---:|---:|---:|
| 之前全部 dedicated | 266.16 | 476.22 | 136 |
| VMA buffer + 独立 Upload，image dedicated | 278.06 | 498.80 | 60 |
| 本轮 VMA buffer/image + 独立 Upload | 272.88 | 493.62 | 16 |

本轮常驻 backing 为 286133340 字节，峰值 517596920 字节；资源范围为 279085388 字节，136 个 ranges（84 buffers + 52 images）。相比上一阶段少 5.18 MiB backing，唯一物理分配从 60 降至 16；相比最早 dedicated 仍多 6.72 MiB 常驻、17.40 MiB 峰值，属于块预留/碎片代价。staging payload 峰值保持 220264128 字节；没有减少上传数据，也不能据此宣称硬件显存/60 FPS 改善。

硬件性能、显存/池策略、非 coherent 和最终画质继续等用户换回 RTX 4060 Ti。此增量不新增 RenderDoc 捕获或宣称 PBR/HDR 主通道已经完成；M2 才交付线性 HDR 场景和统一显示输出。
