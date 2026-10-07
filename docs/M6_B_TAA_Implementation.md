# M6-B：TAA与4060 Ti硬件验证

2026-10-07。用户确认切回4060 Ti，已在沙箱外检测NVIDIA GeForce RTX4060Ti 8GB/615.71.09、Vulkan1.4.351。Codex沙箱隐藏/dev设备，沙箱内NVML失败不代表驱动坏。原生X11路径完成验证；Wayland测试首次surface尺寸变化导致OUT_OF_DATE，尚未将这条路径标为完成。VK_LAYER_KHRONOS_validation未安装，本文不宣称validation layer验收通过。

依据与选择见[M6-B决定](M6_B_TAA_Decision.md)，参考RTR4第6章及[作者TAA survey](https://research.nvidia.com/labs/rtr/publication/yang2020survey/)。未引入超分辨率或GI/GTAO。

## 实现

独立fullscreen resolve置于线性HDR与公共显示转换之间。两个RGBA16F颜色历史、两个R32F线性视深历史，原生1080p新增49,766,400B像素载荷；实际VMA分配另计。固定两套描述符、自有5个sampler bindings及112B push，主PBR资源额度/608B UBO/128B push保持。每次swapchain重建多1条pipeline（总14含cluster），候选HDR/motion/depth/历史及显示描述符全部成功才替换。当前历史资源也为TAA关闭路径保留，按需分配优化后置。

motion.w从FP16投影深度改存previousClip.w；标准透视下它是线性视深。当前D32由inverse raster VP重建同度量，历史保存R32F。previousUV=currentUV-motion+previousJitter-currentJitter。无历史、无效motion、UV越界、透明有效度不足、天空/几何类型变化及前深度不匹配拒绝；相对深度容差.2%并计像素内坡度，避免非线性深度远景量化。天空深度0、只重投影旋转；不支持的投影绕过TAA。MASK沿用主pass实际discard。

当前3x3邻域YCoCg min/max与均值/方差限制历史，current权重.1，运动增加至.4；不额外锐化。双线性history重建目前有可见柔化，静止细节/快速移动/高光与透明质量不是最终验收，M6-C首先比较更清晰的重建与history confidence/reactivity。GI/AO缺失仍由后续阶段解决。

Graph记录history初始化、读写奇偶RAW/WAR/WAW和展示读依赖；成功queue submit才发布parity/ready，reset不覆写飞行中的图像。scene commit、camera/reset、resize、AA开关、数据debug切换失效；capture不推进历史。CSM未抖动、cluster射线匹配raster jitter、相机剔除留半像素；UI最后绘制。Viewer默认TAA启用，Render Debug有TAA开关，CLI `--no-taa`同时关闭resolve/jitter用于比较，Renderer兼容测试默认关闭。独立每槽2-query池、UI/CSV/summary `gpu_taa_ms`记录resolve边界成本；`taa_enabled`为实际启用。

## 验证

RTX原生X11完整32/32通过（18CPU/14GPU），329.42s。TAA shader GPU fixture证明：首帧不读未定义历史，0→1→0得到0/.1/.09，64交替帧末值.522949；无效/越界/深度/透明拒绝，梯度测试区分jitter补偿和motion重投影，HDR4邻域clamp、reset。补充的两个timeline阻塞提交在GPU尚未运行时发布奇偶状态，独立staging/push得到0/.1，证明history依赖和快照不串帧；对应针对回归2/2通过，软件llvmpipe同一fixture也通过。shader SPIR-V Vulkan1.3校验通过。CLI修复后由真正开/关viewer运行验证。

硬件初次27/31有四个相同MASK断言失败：LOD1 fixture的base nearest取样恰落于texel边界，两驱动选择不同侧；偏移.01UV进入明确texel内部后所有coverage/编辑回退回归通过，未修改材质实现。另外发现新temporal_motion.glsl未列入shader自定义命令依赖，源代码已改而triangle SPIR-V仍旧，造成motion.w读.5；现在自动列入所有.glSL依赖，重编译后新深度及TAA回归通过。首次收敛断言多重复一次亮帧，改为验证真正的交替序列。

本地RenderDoc1.45 C API注入抓取；用已安装qrenderdoc的Python replay API检查actions、targets并导出图像。Qt包仅含xcb插件，采用X11回放。厨房off捕获2079actions/25textures，on2084actions/29textures；on明确存在TAA resolve与R32_FLOAT。截图、.rdc、完整日志/时序在聊天工作目录`outputs/m6b`，不加入模型仓库。仪器化运行与无注入运行分别记录；短时序不是整条画质路线验收，报告仍保持hardware_target_accepted=false。

最终生产指纹`796c02f40809dbbcfad495857c55e204b06d5b91c443f3e10175b957b32ebf4e`，102inputs。全量回归后补充的报告/CLI开关与采样格式guard再通过对应测试/实跑，shader机制未改变。

原生1080p短测（4s暖机/8s采样）：仪器化厨房off990帧、on970帧，TAA GPU p50=.165888ms/p95=.182272ms；四合院orbit1440帧TAA p50=.114688ms，FlightHelmet orbit1440帧p50=.195584ms，所有查询有效且prepared/retired/staging归零。无注入厨房off722帧GPU p50=10.909088ms，on78帧GPU p50=14.036544ms、TAA p50=.182272ms，但on多帧acquire约997ms并伴随GPU低时钟。这组完整记录保留但**不接受为60FPS性能比较**；等待发生在图像获取阶段，尚不能将它归因于TAA。需要在相同可见性/呈现状态下复测，不能剔除慢帧宣称达标。

## 下一步

M6-C：优先减轻双线性历史反复重采样的柔化，以静止细节/慢快轨迹、遮挡显露、远处瓦片/叶片与高光验证更清晰的history重建；细化透明和灯光变化reactivity，曝光保持显示阶段。固定轨迹逐帧测拖影与细节后再进入M7 GTAO。32项通过不等于最终TAA画质通过；当前原生Wayland/validation layer接受项仍开放。
