# M1-C：VMA buffer/staging 适配

本记录保留 buffer/staging 阶段的合同与历史对照；后续图像迁移已完成，当前合同与结果见 [image 适配](M1_C_VMA_Image_Adapter.md)。

2026-10-03。固定 VMA 3.3.0，commit `1d8f600fd424278486eade7ed3e877c99f0846b1`；MIT 及上游 SHA256 见 `deps/vma/provenance.json`。构建无需在线下载，依赖源码进入 source fingerprint。API 依据：[官方固定源码](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/blob/v3.3.0/include/vk_mem_alloc.h)、[映射说明](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/memory_mapping.html)、[统计说明](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/statistics.html)。

## 所有权与映射

Device 的普通 buffer 工厂返回项目 `GpuBuffer`，其私有分配对象管理 VmaAllocation；VMA 类型只出现在实现文件。保留 Vulkan-Hpp buffer handle 的现有绘制接口，先销毁 buffer，再调用 vmaFreeMemory，最后释放资源账本 lease。采用 vmaAllocateMemoryForBuffer / vmaBindBufferMemory，与 Vulkan-Hpp 析构职责配对，避免双重 destroy。

`write/read(span, offset)` 使用分配内相对偏移并检查 payload 范围；通过 VMA map/unmap、flush/invalidate 处理非 coherent atom 对齐，不把 allocation offset 再加一次。Frame/material UBO 和上传不再直接映射 DeviceMemory。CPU 可写 buffer 只要求 host-visible，优选 coherent；调用方仍负责 GPU/CPU 使用顺序和 fence。本轮软件设备实际选中 coherent，非 coherent 硬件路径未验收。

`createUploadBuffer` 单独选择 Upload 生命周期；常驻 buffer 使用 Resident。每类共享自己的 VMA allocator，由该类所有 buffer 持有；Device 工厂只保存 weak 引用。上传完成、staging 释放后，Upload allocator 及其所有空闲块可完整销毁，常驻 UBO 不会延长上传内存的生命周期。多个并存上传仍共享 Upload allocator，直到最后一个 buffer 释放；取消保活继续由 UploadBatch/Renderer 的完成 fence 决定。

Device 必须活得比资源久；现有 shutdown 先等待/加入任务、完成提交并释放 Renderer，再销毁 Device 的顺序保留。没有改变队列调度、提交次数或等待策略。默认 VMA 线程保护保留，适配层锁覆盖分配/释放与 block 记账操作，不覆盖 CPU memcpy，也不代替 queue 外部同步。

## 账本合同（报告 schema 2）

| 字段 | 含义 |
|---|---|
| payload_bytes | buffer/texel 的有效数据 |
| allocations / allocated_bytes | 唯一 VkDeviceMemory 个数与字节：尚未迁移的 dedicated image + VMA backing blocks |
| suballocations / suballocated_bytes | 已迁移 buffer 的 VMA allocation 个数和实际范围大小，归于资源生命周期域 |
| allocator_blocks 域 | 所有 Resident/Upload allocator 的物理块；不属于某个 scene 生命周期域 |
| device_local/host_visible bytes | 对应内存类型属性；两者允许重叠，不能当成两个独立总量 |

物理 backing 与 allocation range 不相加。Buffer 的 physical bytes 由块 lease 单独记录一次；资源域保留自己的 payload、对象与 suballocation。准备/提交/退休只转移资源域，不能整体转移一个被多域使用的共享块。

VMA allocate/free callback 跟踪每个 memory handle。free callback 发生在 vkFreeMemory 前，因此先标记、在 VMA 操作返回后再释放 lease；适配层锁避免另一线程提前清除。C callback 不抛异常，注册失败在创建操作返回后报告并回收候选。统计交叉检查在静止检查点将 VMA blockCount/blockBytes/allocationCount/allocationBytes 与账本比较；异步运行中的两次独立采样不承诺同一时刻。

空闲块可能保留到所属 allocator 的最后一个资源释放。失败/取消必须恢复资源和 suballocation 基线；不能把“缓存块字节必须立即等于旧值”当成通用断言。最终 Renderer/Application shutdown 和 allocator 销毁仍要求全部归零。Driver heap、swapchain、ImGui backend、驱动开销与 CPU 资产仍独立或排除。

## 软件检查与内存代价

10 项 CTest（8 CPU、2 llvmpipe/X11 GPU）全部通过。新增检查包含两小 buffer 共享 2 MiB block/独立 8 KiB ranges、非零分配偏移及非对齐部分读写、越界拒绝、移动赋值、32 个并发 buffer、VMA 统计、上传块与常驻块隔离、最终归零。原像素回读/共享缓存、pending upload gate、取消/退休、真实 ImGui 和准备中退出保持通过；4 commits / 6 frames / 5 scene uploads / 6 image copies / 0 runtime upload waits。

四合院 27 meshes / 27 materials、1920×1080/UI/orbit 短场景数据如下。payload 全部为 278892320 字节；不是硬件显存/帧率结论。

| 软件配置 | 常驻 backing MiB | backing 峰值 MiB | 常驻 VkDeviceMemory 个数 |
|---|---:|---:|---:|
| 之前 dedicated buffer | 266.16 | 476.22 | 136 |
| VMA 混合上传/常驻生命周期（排查对照） | 326.06 | 484.80 | 63 |
| 最终隔离 Upload 生命周期 | 278.06 | 498.80 | 60 |

隔离生命周期降低 48 MiB 常驻 backing，避免上传留下的空间受常驻资源牵制；最终比旧实现仍多约 12 MiB，峰值多约 23 MiB。VMA 池预留/碎片有成本，不宣称减少了总显存。当前 preferredLargeHeapBlockSize 为 16 MiB；这不是总内存上限，初始块可更小、独占的大分配可超过该值。分配策略调优等 4060 Ti 实测。staging 有效数据峰值仍是 220264128 字节，子分配不会减少上传数据；节流另立增量。

Validation layer 本环境不可用，未新增 RenderDoc 捕获；硬件性能/VRAM、非 coherent 和最终画质按用户要求暂缓。烟测仅证明正常加载/绘制/退出与报告字段。

## 后续已交付

Texture/HDR、depth/shadow image 已迁移到同一 GpuAllocator，完整资源范围与唯一 backing 分开记账；views/samplers、共享缓存、resize、取消/退休与最终归零回归通过。见 [image 适配合同](M1_C_VMA_Image_Adapter.md)。下一增量进入 M2 线性 HDR 场景与统一显示输出，然后逐步接入最小 Render Graph。
