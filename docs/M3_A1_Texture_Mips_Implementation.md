# M3-A1：typed mip 与采样实施合同

2026-10-04。图形代码前参考 real-time-rendering-advisor，替代方案和官方依据见 [决定](M3_A1_Texture_Mips_Decision.md)。

## 过滤与资产绑定

`texture_mip.hpp/.cpp` 是无 Vulkan 依赖的 RGBA8 过滤模块。尺寸/总字节数检查溢出；原始 LOD 0 原样保留；每级 floor/2 到 1×1。面积 box 覆盖奇数尺寸末行/列，不把 3×5 简单裁成 2×4。颜色 RGB 在线性空间过滤再存回 sRGB，alpha 独立线性平均；MR/AO/height/alpha 的通道分别平均。Normal 策略平均解码向量并归一化，近零回退 +Z。double 累加、8-bit 存储，不宣称保留法线方差或物理 BRDF roughness 过滤。

MaterialGpuStore 明确为 normal 选择 Normal，颜色/数据选择 Average；alpha mask 的 baseColor/独立 alpha 选择 BaseOnly。cutoff/factor 和 vertex alpha 的覆盖率合同属于 A2。BLEND 使用非预乘 alpha；透明边缘扩色/过滤仍需后续验收。HDR 环境继续 RGBA32F 单层，未把普通 mip 当作 GGX 预过滤。

## GPU 资源和采样器

完整链打包到一份 staging，UploadBatch 校验连续、紧密排列的 RGBA8/RGBA32F 布局，生成逐级 copy region，全链 Undefined→TransferDst→ShaderReadOnly barrier。每图像仍只计一次 imageCopies；payload/上传 bytes 则包含所有层。CPU decoded LOD 0 在创建 staging 前释放。后台准备/主线程单次提交、原子 upload ticket、就绪轮询、取消保活及退休合同不变。init/tests 的 finish 仍允许等待。

image/view 覆盖完整级数；缓存键包含内容、color space、Average/Normal/BaseOnly、算法版本，sampler 请求独立缓存。即使绑定使用非 mip sampler，普通图像仍生成完整链，使不同 sampler 能共享同一图像。相同源用于 normal 与 packed data 要分开，即使某张图只有一层，也保留用途身份；当前版本不会在不同策略之间尝试物理表示去重。

显式 glTF min/mag/wrap 不变，省略 filter 的默认策略改为 linear/trilinear。mip sampler 的 maxLod=VK_LOD_CLAMP_NONE；non-mip 的 nearest mip/maxLod=.25 保留 min/mag 区别。Device 仅在支持时启用 samplerAnisotropy，8x 请求受已启用 feature/limit 限制；所有 mag/min/mip 都是 Linear 才启用，nearest/non-mip 忽略各向异性请求，maxAnisotropy=1 可关闭。TextureResources 提供 mipLevels/maxAnisotropy 以检查实际资源元数据。

## 验证

Release/Ninja 构建成功，18/18 CTest 通过（11 CPU + 7 llvmpipe/X11 软件 Vulkan，未 skip）。CPU 数值回归和 ASan/UBSan 检查通过：黑白颜色 mip≈188、数据/alpha≈128、packed 通道、法线单位长度/零向量、奇数/长条尺寸、offset/overflow/BaseOnly。测试 shader 的 SPIR-V validation 通过。

软件 Vulkan 使用生产 loader/cache/upload：UNORM/sRGB/Normal 全层和 3×5 NPOT 逐层回读一致；RGBA32F raster probe 验证高 LOD、trilinear、全部六种 min 模式、mag 和 repeat/clamp/mirror。默认 8x 与 device-limit 请求的元数据和关闭/nearest/non-mip 策略通过，未测物理 GPU 上的各向异性画质/成本。四张不同用途图像上传为单批，完整 payload=76 bytes，view 覆盖两层；最终账本归零。

既有场景事务仍为 4 commits / 6 frames / 5 uploads / 0 runtime upload waits。首场景 image copies 从 6 变 7：fixture 将同一图片用于 normal 与 packed data，新策略明确分开。重复切换仍零新增图像复制。取消、失败回退、UI preview 退休、HDR/图状态、一/两帧、KHR/EXT/legacy、resize/shutdown 回归通过。

软件命令显式指定 llvmpipe、取消 WAYLAND_DISPLAY，并设置 MESA_VK_WSI_DEBUG=sw,noshm；生产 run.sh/应用未强制此环境。原生 MIT-SHM/DRI3、legacy release proof、validation、新 RenderDoc、4060 Ti 运动画质/60 FPS/峰值 VRAM 验收继续开放。下一项 A2 为 alpha coverage 与 tangent/法线空间合同，随后推进 GGX prefilter/BRDF LUT。

源码 SHA256 `643a06952fb6ff1b518fdae405d17855f5c2ef80b16c0e01b5cc3402990b8627`。本增量与 M2-B 至 M3-C1 纳入同一次集成提交。烟测、内存差值和任务 patch 的记录位于聊天工作区 outputs。


## 四合院与报告结果

报告 one/auto、two/fence、two/legacy 分别 18/20/17 个完整有效查询。四合院原生 1920×1080、UI/orbit/FIFO/require-fence 一/两帧各 4 行/查询/入队/完成，pending=0、release proven=true。staging/prepared/retired suballocation 全部为零。构建头和两个报告与上述源码指纹一致。

| 当前资源 | 一帧 | 两帧 | 相对 M2-C2 同配置 |
| --- | ---: | ---: | ---: |
| images / views | 53 / 53 | 53 / 53 | 不增 |
| samplers | 5 | 5 | -1（省略 filter 的新默认与相同显式配置合并） |
| buffers / descriptor sets | 84 / 29 | 85 / 30 | 不增 |
| texel/buffer payload bytes | 340,115,120 | 340,115,568 | +44,634,000（42.57 MiB） |
| suballocation bytes | 340,464,716 | 340,465,164 | +44,790,528 |
| backing blocks / bytes | 20 / 367,093,084 | 20 / 367,093,084 | +3 / +64,370,944（61.39 MiB） |

本次加载期间 backing 高水位 646,085,040 bytes（616.15 MiB）；包含 staging，但不含 CPU decode/mip/cache 内存、swapchain/ImGui 后端和 driver overhead，不等于总 VRAM。两帧仍只多一个 448-byte frame uniform 与 descriptor set。

首次完整场景加载分别约 2.19/2.22 秒，上一轮单次记录约 1.10/1.11 秒。这个差值是软件环境加载代价的观察，非受控性能 A/B，不能全部归因于 CPU mip 或用来预测 4060 Ti。后续加载测量应分离 decode/filter/staging/submit，并评估缓存或离线资源；本轮未为未测量的 GPU 瓶颈增加 compute/multi-queue。
