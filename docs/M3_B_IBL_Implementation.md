# M3-B GGX 环境反射实现

本次把单张经纬图与 SH 的粗糙度混合近似替换为 GGX 预过滤 cubemap + split-sum BRDF A/B LUT。保留原始 2D HDR 天空；SH 继续提供漫反射。先按 [决策](M3_B_IBL_Decision.md) 比较 CPU/compute/离线方案、确定采样/资源/同步/缓存与验收，再实现。

## 数值与资源合同

- `hdr_ibl` 是纯 CPU 模块：线性非负有限 HDR；感知粗糙度 r、GGX α=r²；Hammersley 重要性采样，N=V=R、NoL 归一化卷积；每 mip 独立烘焙。默认 cube 128²×6、8 mip、512 样本，LUT 128²、1024 样本。BRDF 用 Karis IBL k=α/2，着色器使用 F0·A+B，不再额外乘 roughness Fresnel；直接光保留其原有可见性近似。
- 源图先 2×2 分层转换为 cube。源 cube 边长取 `bit_ceil(clamp(max(2*输出边长,源宽/4,源高/2),2,1024))`；生成准确立体角加权 mip，采样跨面邻接 texel。GGX PDF 与输出纹素足迹决定源 LOD。数值测试发现经纬图按面积选各向同性 mip 会过度模糊极点，该方案已被 cube 源表示替换。有限源分辨率、四点转换和 FIS 仍有过滤误差；任意小于纹素的极亮光源不能宣称精确还原。
- cube 面顺序/方向遵循 Vulkan +X/-X/+Y/-Y/+Z/-Z。粗糙度 LOD=r·(textureQueryLevels-1)，完整 view/trilinear、clamp 三轴、无各向异性；LUT 为 clamp/linear/no-mip，横轴 NoV、纵轴 r。天空保留水平 repeat，垂直 clamp。
- CPU 用 double 累加。SH 按每个经纬纹素的球面矩形准确积分九个基函数，小常量图不会产生虚假的高阶系数；GPU 使用既有 irradiance/π 合同。cube/LUT 先用 RGBA32F，LUT 的 RG 存 A/B；不烘入曝光、强度、旋转或 AO。
- 新增两个不可变 Persistent 图像、视图、sampler；精确 payload：cube 2,097,120 B、LUT 262,144 B，合计 **2,359,264 B**。初始化把原天空、cube、LUT 三个图像放入同一 UploadBatch：3 image copies、1 submit、1 cold fence wait。检查 sampled/linear 格式能力及 cube/2D 维度上限。每个 copy 覆盖六层和完整 mip，barrier 覆盖所有 mip/层，transfer write→fragment sampled read；初始化完成后状态保持只读。
- 帧描述符 binding 4/5 为 cube/LUT，原 binding 3 仍供天空使用；每帧 pool 为 5 combined image samplers。**448-byte UBO 不变**。环境上传统计仍表示一轮初始化，实际图像数在启动日志单独列出。场景准备、弱材质纹理共享、主线程提交/就绪和退休均沿用原链路。

## 磁盘缓存与失败处理

`hdr_ibl_cache` 是纯 CPU 文件缓存，默认 `.cache/ibl`，相对于现有资源工作目录；目录已被 gitignore 排除。没有加入运行时环境替换或自动探针刷新。

算法版本 1 的身份包括方向/源过滤/BRDF/SH/格式约定、源维度和完整解码 float 位模式、cube/LUT 尺寸和各自样本数。后续改变这些固定语义必须增加版本。FNV64 文件名只用于定位；读取逐字节比对完整身份，不能因哈希碰撞复用错误数据。文件有 magic、预期长度、显式 little-endian 编码和 payload checksum；长度先按可信参数计算，不按文件里声称的长度分配。SH、cube、LUT 一起缓存。校验不是对抗恶意修改的认证机制。

缺失、旧身份、损坏、截断重新烘焙；目录不可写则继续使用 CPU 结果。每个写者通过原子创建独占临时目录写完整文件，再 rename 发布，退出清理临时目录。缓存不引用 GPU 对象；窗口退出/场景退休不会被缓存延迟。完整源身份也写入文件，所以默认 2048×1024 HDR 的一个缓存约 34.25 MiB；这是避免哈希别名的明确空间取舍。可删除 `.cache/ibl` 重新烘焙。

## 验证记录

CPU 测试验证每面/每 mip 的常量 HDR（RGB=4/1/12）、小图 SH 的 π 倍 irradiance、方向梯度/极点/面边界、高亮小光源展宽、白炉 A+B、r=0 Schlick 极限及 r=1/NoV=1 的 1-ln2。另用不采用 GGX/Hammersley 样本的半球网格积分对照多个 r/NoV。缓存测试覆盖位一致命中、全部参数/源维度/源内容变化、完整身份、checksum/截断、不可写回退与并发发布。ASan/UBSan 通过。

真实软件 Vulkan 回读六面全部 mip 和 LUT 的每个 float，与 CPU 上传数据完全一致。生产 triangle shader 验证六轴、旋转、粗糙度层间插值、HDR >1、金属/电介质及 NoV=.1/.5/1、面边/面角、SH 漫反射/金属零漫反射、EV 不改 scene HDR 和环境强度线性。GPU 数值夹具采用与 viewer 一致的 Vulkan Y 翻转；掠射角的 reflection 会切换 cube 面，预期值按该方向构造。片元输出是 RGBA16F，允许 max(.008,.003·expected) 的半精度/插值误差。

最终 **20/20 CTest（13 CPU + 7 软件 Vulkan，无跳过）** 通过，原有四种 GPU 场景事务保持 4 commits/6 frames/5 scene submits/7 scene image copies/0 runtime fence waits，退出资源账本归零；环境/管线仍为 1/12。报告 smoke 一帧/auto、两帧/fence、两帧/legacy 分别 1/10/11 行，与 GPU queries 匹配；这是短时 schema/生命周期检查，不能解释为稳定吞吐测量。

四合院一/两帧，1920×1080、UI/orbit/FIFO/fence 各 4 完整帧/query/present，pending=0、扩展释放证明成立，staging/prepared/retired=0。两者当前均为 55 images/75 views/8 samplers/21 backing blocks，backing=377,220,480 B，峰值=670,534,136 B。payload 一帧=350,625,684 B、两帧=350,626,132 B；两帧增量仍是 448-byte UBO。相对 A2，新环境资源 payload +2,359,264 B、suballocation +2,361,856 B，新增一个 8 MiB backing block（+8,388,608 B）；活跃场景/共享材质 payload 不变。账本不含 CPU、driver、swapchain、ImGui 额外开销，不是总 VRAM/驻留测量。加载单次观察约 3.52/3.51 s，不作受控性能归因。

本聊天 `outputs/m3b-verification.json`、`m3b-final-test-details.log`、sanitizer、报告和 task-only patch 保留证据。生产 source SHA256 `622633fa3e7680b2074880442c7dcd8ac5da5316d39ff9acf08db5c3d2f2ee5a` 与生成 header 和四合院报告一致。Shader 使用 glslang 编译与 spirv-val 检查。测试环境是 llvmpipe/X11 + 显式 `MESA_VK_WSI_DEBUG=sw,noshm`，正常启动不强制软件环境。

## 保留边界

split-sum 的 N=V=R 假设不能精确表示掠射角各向异性；单次散射会在高粗糙度丢失能量，SH 有频带截断，全局天空没有局部遮挡/室内反射。局部探针/SSR 属 M8；必要材质 specular 扩展和镜面抗锯齿属 M3-C。4060 Ti 时间/VRAM、原生 WSI、运动画质、validation 与新的 RenderDoc 验收继续等用户换机通知。本次没有物理 GPU 探测或新抓帧；本增量与 M2-B 至 M3-C1 纳入同一次集成提交。
