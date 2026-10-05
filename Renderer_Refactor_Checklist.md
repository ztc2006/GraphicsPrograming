# Renderer 阶段执行清单

更新：2026-10-04。当前入口：M3-A typed mip/过滤/alpha coverage；M2-C2 扩展呈现生命周期软件增量已交付；M2-A 线性 HDR/统一显示输出与 M2-B 最小单队列图已交付软件实现；M1-C cgltf 与 VMA buffer/image/staging 适配已交付。正式范围与依赖以 [Engine_Roadmap.md](./Engine_Roadmap.md) 为准；代码依据见 [Renderer_Design_Review.md](./Renderer_Design_Review.md)。

旧生命周期重构方案已归档到 [完整旧版](./docs/archive/Renderer_Refactor_Checklist_before_2026-10-02.md)，其中先销毁旧资源、Windows launcher 和三角形验收等规则不再生效。

## M0：真实硬件与图像基线

状态：测量基础和软件设备功能证据已交付；硬件与画质验收按用户要求延后，等其通知换回 RTX 4060 Ti 后再执行。

- [x] 记录实际运行设备、驱动、Vulkan API、堆容量、构建和 validation 启用状态；报告区分软件设备。
- [x] GPU timestamp 按提交帧匹配、处理有效位/回绕；加入 Frame/Shadow/Main/UI 标签与资源名称。
- [x] 输出 CPU 准备、fence/acquire/submit/present 时间，以及 GPU total/shadow/main/UI 的 CSV 和 p50/p95/p99。
- [x] 记录初次场景加载耗时；支持 memory-budget 采样，包含新旧资源并存点，明确采样堆用量不是精确应用显存。
- [x] 固定 static/orbit 相机路径、原生 framebuffer 尺寸校验、配置记录与 C++/shader 源码 SHA-256。
- [x] 加入金属/粗糙度、UV 高频、alpha mask/blend、镜像单面几何诊断输入；保留 UV/矩阵、HDR 数据等语义回归。
- [x] Release 原生 1080p 软件设备短测：材质场景 78 帧、四合院 orbit/UI 7 帧，GPU 查询全部匹配；图形退出回归通过。
- [x] RenderDoc 1.45 实际抓帧并验证标签/资源，保存材质场景图像；作为功能与缺陷基线。
- [ ] RTX 4060 Ti 实际显存/驱动/能力、validation layer 检查；可用 layer 下无错误。
- [ ] RTX 4060 Ti 普通运行验收四合院和 Sponza_2；检查启动/UI/拖放/Reload、失败保留旧场景、连续切换及 resize。
- [ ] 至少三次 30 秒预热 + 120 秒稳态：原生 1080p Release，吞吐/VSync 分开；接受硬件帧预算与显存余量。
- [ ] 录制固定路径运动画质，建立 sampler/HDR/透明和镜像几何的参考对照；当前诊断图不能作为已正确的 PBR golden image。

之前的 M0 执行环境未暴露硬件设备，软件设备没有代替硬件验收；当时的 `VK_LAYER_KHRONOS_validation` 不可用。本轮依用户要求不再排查物理设备访问，开发继续推进。命令与报告字段见 [README](./README.md#repeatable-m0-measurements)。

## M1：资产生命周期与上传

- [x] M1-A：用候选 GPU 资产集合替代候选完整 Renderer；上传完成后提交，失败保留旧场景。
- [x] M1-A：场景资产与环境、流水线、frame context 解耦；现由 M1-B 在使用旧集合的帧完成后清理预览并释放旧集合，提交本身不等待。
- [x] 软件 Vulkan 事务回归：4 次提交、6 帧，损坏纹理/非法网格/提交拒绝后仍可绘制；环境上传保持 1 次、流水线构建保持 8 次，UI 回调和帧编号连续。
- [x] M1-B：每次场景准备只提交一个 UploadBatch，用一个 fence 确认完成并释放 staging；准备失败不提交半批资源。
- [x] M1-B：单后台任务准备、主线程提交/就绪轮询、按完成帧退休旧集合与预览；运行时加载不显式等待上传/旧帧 fence。
- [x] 应用回归：连续请求、失败回退、上传前后取消、真实 ImGui 预览退休、准备中退出；未提交候选缓存隔离和未完成上传强所有权。
- [x] 单 mip 的图像/颜色空间视图、sampler、material binding 分离；相同编码内容去重，文件原地修改产生新图像，弱缓存不持有闲置 GPU 资源。
- [x] glTF 每贴图 sampler/filter/wrap 解析与非法值回归；实际 mip 链、typed mip 用途与各向异性仍属 M3。
- [x] 精确资源账本：有效数据与实际 allocation 字节分离；独立记录持久、准备/使用/退休场景私有资源、共享纹理与 staging 当前/峰值；共享对象不重复计数，保留单独 driver heap 采样。
- [x] 账本回归：后台并发注册、失败回退、取消中保活、旧集合退休、重复加载共享稳定、Renderer 销毁归零；报告与 UI 暴露统计，硬件结果等待换机。
- [x] staging 批量上传与完成 fence 替代每张纹理单独 queue.waitIdle；旧 Device 单次复制接口已移除。
- [x] 软件 Vulkan 扩展回归：5 张不同图像单批上传，GPU 像素回读一致，采样器独立、文件/内嵌内容共享、文件修改失效、重复场景零新增图像复制。
- [x] 比较 cgltf / fastgltf / TinyGLTF 当前主线及 VMA；固定 cgltf 1.15、MIT/provenance，库类型留在项目适配层，构建不下载依赖。见 [选择记录](docs/M1_C_Library_Decision.md)。
- [x] sparse/normalized、交错基础+sparse、精确 sparse 索引、原始 tangent/handedness、独立 UV/变换、无 material 的默认材质与错误输入回归。
- [x] required 扩展拒绝策略；optional 扩展警告随场景提交到 UI，失败保留旧场景与警告。10 项 CTest（含 2 项软件 Vulkan）及 ASan/UBSan 导入回归通过。
- [x] VMA 3.3.0 buffer/staging 适配：项目所有权、映射/flush/invalidate、按 Upload/Resident 隔离生命周期、唯一 backing 与资源 range 分开记账；schema 2，保持异步/取消/退休/归零。
- [x] 分配回归：共享块/偏移读写/越界/移动/并发/VMA 统计与上传块完全回收；10 项 CTest 与场景/报告烟测通过。见 [适配合同](docs/M1_C_VMA_Buffer_Adapter.md)。
- [x] image 分配迁移：Texture/HDR、depth/shadow 使用公共 GpuAllocator；独立 view/sampler、默认 granularity、混合范围、RGBA32F 回读、失败/最后 image 归零与 3 次真实 resize/绘制通过。10 CTests、34 帧报告与四合院烟测；见 [图像适配](docs/M1_C_VMA_Image_Adapter.md)。
- [x] M3-C1 软件接入四合院必需 KHR_materials_specular；未知扩展的 required/optional 诊断继续保留，最终材质画质等 4060 Ti 验收。

## M2：线性 HDR 与通道资源

- [x] M2-A：RGBA16F 主场景、天空、透明线性合成与统一 EV/filmic/sRGB 输出；环境强度独立，数据 debug 绕过显示曲线，UI 后绘制。实现前使用实时渲染技能，见 [决定](docs/M2_A_HDR_Decision.md)。
- [x] M2-A：四种 sRGB/UNORM RGBA/BGRA、5 组显示设置数值回读；实际 PBR emissive/alpha blend 保留 >1、天空 EV/强度分离、非法 EV/nested frame 拒绝、输出构建失败保留旧资源、3 次 resize、最终账本归零。
- [x] M2-A：10 项 CTest、24 帧完整 output 查询报告与四合院原生 1080p/UI/orbit 烟测；报告 schema 3，HDR 增加 15.82 MiB。见 [实施合同](docs/M2_A_HDR_Implementation.md)。
- [ ] M2-A：换回 RTX 4060 Ti 后补新 RenderDoc 抓帧、画质与输出 pass 成本；本轮直接 Vulkan 1.3 软件设备注入未通过设备选择，未宣称抓帧成功。
- [x] M2-B：阴影、主场景、输出、独立 LOAD/STORE UI 迁移到最小单队列 Render Graph；Application 一次提交场景 draw list，实际 resource/pass/barrier dump 可检查。见 [决定](docs/M2_B_Graph_Decision.md) 与 [合同](docs/M2_B_Graph_Implementation.md)。
- [x] M2-B：主深度 Sampled/TransferSrc + STORE/read-only export；状态仅提交成功后发布；RAW/WAR/WAW、内容有效性、UI 同布局保留、初次禁用阴影/开关、失败与 resize、异步资产/退休和最终归零通过，11 项 CTest。
- [ ] M2-B：当前 WM 未确认原生 iconify；显式 restore/recreation 已通过，原生最小化/零尺寸等待、validation、RenderDoc 和真实同步/带宽成本待换机补验收。
- [x] M2-C1：启动一帧/两帧配置；独立 uniform/descriptor/command/query/acquire/fence，成功 submit ID、完成逐项交付、最高 ID/UI 与陈旧 fence 清理，schema 4。见 [合同](docs/M2_C_Frames_Implementation.md)。
- [x] M2-C1：13/13 回归（9 CPU/4 软件 GPU）、pending 两帧真实 uniform 像素、查询不丢失/不重复、逆序槽位复用、退休/resize/归零；30/24 帧报告与四合院各 5 帧/query，两帧 payload +448 bytes、图像/backing 不增。
- [x] M2-C2：KHR/EXT instance/device/feature 能力协商，auto/fence/legacy，SwapChain 每图像 semaphore/fence，Renderer 重建保持呈现对象，入队/拒绝错误记账与 resize/exit drain。见 [合同](docs/M2_C2_Presentation_Implementation.md)。
- [x] M2-C2：17 项 CPU/软件 WSI 回归，KHR/EXT/legacy、一/两帧、13 请求/query、pending/复用/幂等 drain、OOM 注入不等待未提交 fence、真实 resize 与归零；UI/报告显示证明边界。测试环境显式使用 Mesa sw,noshm。
- [ ] M2-C2：未扩展 legacy 释放证明仍未关闭；当前隔离会话原生 MIT-SHM/DRI3 FenceFromFD 顺序故障、真实 WSI OUT_OF_DATE/device-lost、原生最小化、validation 与 4060 Ti 现场验收待补。
- [ ] 换回 4060 Ti 后比较一帧/两帧真实吞吐和延迟，决定默认配置；历史资源显式管理。

## M3–M5：材质、光照和阴影

- [x] M3-A1：CPU typed mip（sRGB/线性数据/法线）、奇数尺寸边缘、全链单 staging/copy/view、带用途/版本的共享身份；六种 min/mag/wrap 与受 feature/limit 限制的各向异性。见 [决定](docs/M3_A1_Texture_Mips_Decision.md)。
- [x] M3-A1：18/18 CPU/软件 Vulkan 回归、ASan/UBSan、SPIR-V validation；真实 mip 回读/六种过滤/LOD/wrap、异步/取消/退休/归零，四合院一/两帧报告一致。见 [实施](docs/M3_A1_Texture_Mips_Implementation.md)。
- [x] M3-A2：单来源 alpha coverage/阈值身份、共享 LOD 0 编辑/复合/vertex/POM 回退、COLOR_0 alpha、固定 Mikk seam、双面法线与负缩放单面剔除；19/19 CPU/软件 Vulkan、sanitizer、生产 main/shadow/normal 像素与四合院报告通过。见 [实施](docs/M3_A2_Material_Implementation.md)。
- [x] M3-C1：specular 因子/双贴图/独立 UV/sampler、直接光/IBL 的 F0/F90 与 scalar diffuse、UI debug 和参考球；20/20 软件测试、ASan/UBSan、SPIR-V、四合院报告与归零通过。见 [实施](docs/M3_C1_Specular_Implementation.md)。
- [ ] M3-C2：法线方差过滤/镜面抗锯齿；运动画质和 GPU 成本等换机验收。
- [x] M3-B：GGX 环境预过滤、BRDF LUT、准确 SH 投影与版本化磁盘缓存；20/20 CPU/软件 Vulkan、sanitizer、六面/mip/LUT 回读、生产 PBR 视角/旋转/能量、四合院报告与归零通过。见 [实施](docs/M3_B_IBL_Implementation.md)。
- [ ] M3：材质球与四合院最终参考画质、运动缝隙/高光与 GPU 时间等 4060 Ti 验收。
- [ ] M4：普通多灯 forward 先作为正确性对照，再接 clustered light culling；低灯数成本与溢出有回退。
- [ ] M5：稳定 CSM、边界混合与偏移测试；四盏重点聚光灯阴影额度及投影者剔除。

## M6–M8：运动画质和局部效果

- [ ] M6：相机/物体运动矢量、jitter、历史拒绝与 clamp；透明、灯光变化和曝光变化测试。
- [ ] M6：场景切换、相机跳变、resize 清历史；固定轨迹检查拖影与细节损失。
- [ ] M7：GTAO 与滤波，正确调制间接光；可控 Bloom 与可关闭自动曝光。
- [ ] M8：静态/按需局部探针、预过滤、箱体视差校正、混合与更新状态。
- [ ] M8：SSR 深度层级与置信度、时间稳定和 probe fallback；镜面来源不重复累加。

## 每次验收记录

写图形代码前使用 `real-time-rendering-advisor` 核对资源、精度、同步、替代方案与可否证的验收，再实现。记录输入资产、提交/源码状态、驱动、构建类型、分辨率、相机路径、环境强度/曝光和质量档。保留开关对照、相关测试、GPU pass 时序、p50/p95/p99 与峰值显存；不能用软件 GPU 抓帧时间宣称 RTX 4060 Ti 达标。

单个增量以 1–2 周可运行交付为目标，较大阶段拆分。主线之外的 GI、RT、GPU-driven、bindless、异步队列或虚拟几何只在需求/测量重新支持时立项。
