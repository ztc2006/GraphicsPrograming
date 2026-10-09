# 厨房光卡、局部反射与重复面修复

2026-10-09。用户要求修复已定位的窗口单向可见、微波炉反射穿漏和重复墙面；[决策](Kitchen_Repair_Decision.md)，原诊断证据在本聊天outputs/kitchen-geometry。

## 已改变的行为

- 查看器导入后识别明确的`extras.pbrt.area_light_radiance_rgb`元数据，将4张转换光卡标记为primary不可见、shadow不可投影；普通emissive保持可见。完整实例仍参与motion identity、资产退休和probe捕获，光卡发光仍留在捕获中。窗户内侧不再被发光板挡住，外侧继续可见室内。没有全局翻转法线或禁用背面剔除。
- 原始glTF/bin不改。查看器准备阶段删除219个零面积triangle及6个同primitive、双面、不透明、无纹理且着色等价的反向重复triangle。mesh/material/instance ID、顶点属性、原保守bounds保持；透明层、单面反向层、纹理覆盖层和不等价法线保留。零面积全primitive维持原导入/空几何合同，不制造新的空buffer。清理重复运行幂等。
- 原房间probe旁增加可选的一个小型镜面probe：kitchen preset将捕获点放在微波炉内腔，包围区域匹配内腔，内部点优先用这个cube/box projection，外部继续room/global。roughness=.1、metallic=1、几何法线和直接光保持；room SH diffuse不由detail替换。需要时通过Lighting的Small reflection region与共用Capture/refresh按钮编辑/重捕获；切换场景会禁用旧detail配置。

这解决当前固定厨房内部直接采到房间地板的问题，仍是静态/按需局部捕获，非完整RT、SSR、多次镜面传输或动态GI；改变几何/灯/区域后需刷新。glass仍是core alpha近似，玻璃正面的合法房间反射保留。一般自交/近共面不同三角划分没有被批量猜测修复。

## 资源/生命周期

原main UBO608B/push128B、AO112B/20B-per-pixel、pipeline/query/frameGraph目标数保持。IndoorLighting std430原1040B末尾追加detail min/max/position三个vec4，变为1088B/slot；每slot在fence后独立更新。现有cube-array sampler binding4容纳global/room/detail三层，最小cube边128时额外层载荷2,097,120B，非driver heap。TextureLoader/UploadBatch同步扩为最多3cubes/18faces，仍验证精确mip布局/字节数、正方形面和设备layer limit。最初两个guard拒绝三层的失败保留为验证记录，随后完整上传通过。

capture先drain所有frame用户，用相同场景远截面依次捕获room/detail各6face，在direct-only捕获中保留光卡，全部bake/upload成功后才发布新数组/region/key；无效参数/失败保留旧probe。捕获不获取swapchain或推进常规frame ID/history/query。caster属性进入probe几何身份，primary隐藏不改变完整实例集合。

## 证据

新viewer preparation行为先用no-op跑红，再通过功能回归：等价面删除、ID/顶点保持、幂等、保留有意义的单面/透明/纹理/不等价法线、普通发光对象不隐藏；元数据测试覆盖普通emissive、字符串伪key、非法向量长度、嵌套其他对象和转义字符串。CPU准备ASan+UBSan通过（sandbox下leak detector关闭）。

真实kitchen原文件SHA-256与固定下载manifest一致。optional厨房fixture正好得到219/6/4；单独光卡主/阴影draw=0，probe SH有发光能量。完整窗口主深度恢复far，中心HDR由原白板约1.01变为天空(.4475,.5005,.6113)。源码只按元数据识别角色，不按mesh295编号驱动生产逻辑；测试以固定资产核实。

微波炉同camera、无门玻璃的内部镜面控制：room-only采样范数.258934，detail范数.00004837，原错误地板纹理不再来自room cube。保留玻璃的完整画面仍有合法表面反射。detail开/关的direct debug全图最大差0；normal中心仍+X；双slot读取flag/bounds互不覆盖。无效detail捕获保留已发布probe及常规frame ID。

最终完整原生X11/RTX4060Ti/NVIDIA615.71.09/KHR CTest **39/39，64.11s**（20CPU/19GPU）；新厨房1/2slot用例5.39/5.31s。初次37项36/37，一个通用2slot呈现120s超时；原时限单测46.47s通过，最终整套通过，全部日志保留。不能据此宣称此前~1s呈现慢状态彻底解决。相关GPU SPIR-V Vulkan1.3通过，差异空白检查通过，无残留debug instrumentation。源指纹`0673742d3082f032a5eb2b8029798acbbfdb281662e5b65a5c7ce6c22c23f944`。

原生1080p普通查看器短测/实际detail状态、frame query、资源归零记录在本聊天outputs/kitchen-fix/profiles.json与profile-*/summary.json/frames.csv。本轮未做正式30s warmup+3×120s目标接受，validation layer/Wayland仍待。



| 正常查看器1080p短测（5s预热/10s采样） | 完整GPU samples | GPU total p50 ms | AO p50 ms | detail有效 | slow acquire帧 |
|---|---:|---:|---:|---|---:|
| kitchen-on | 1104 | 8.622 | 1.223 | True | 0 |
| helmet-on | 1800 | 1.856 | 0.412 | False | 0 |

两个短测全部query有效，pending presents/prepared/retired/staging均零；正常viewer已验证kitchen detail有效、helmet detail关闭。跨运行差不作因果性能结论。

## 使用

照常通过run.sh打开原始`assets/models/pbr_kitchen/source/kitchen_core.gltf`，auto/kitchen preset默认应用修复与内腔probe；无需替换原资产。资产与shader/场景信息改变后用Lighting Capture/refresh probe。原始光卡/原始几何仍可通过底层raw importer与明确诊断control查看。

新增CPU/viewer准备模块有单独CTest；固定厨房资产存在且开启VULKAN_GPU_TESTS时，自动注册`gpu_kitchen_preview_repair`和`gpu_kitchen_preview_repair_two_frames`，可用ctest复现这三个问题的完整功能闸门。

下一个能力任务仍为M8-A CPU对象BVH；更广内容/玻璃/多次反射与M6-C运动/长时间性能接受独立保留。代码未提交/未推送。图像来自Country Kitchen（Jay-Artist，CC BY3.0），原许可/归档记录保留。
