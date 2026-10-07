# M4-B：GPU clustered forward

2026-10-05。本地软件增量；目标硬件仍为 Linux / RTX 4060 Ti、原生1080p / 60 FPS。
实施前依据及取舍见 [决定](M4_B_Clustered_Decision.md)。

## 交付行为

默认请求 clustered，`--light-culling full` 保留 M4-A 逐灯路径；UI Lighting 中的
`Clustered Lights` 可切换。64×64 framebuffer 分块、24 个对数视深度切片，
每个分区最多64灯。相同灯序、BRDF、材质AA、HDR/曝光和IBL保持可对照。
零局部灯不运行compute；无效/不支持的投影、资源上限或计算能力不足时完整回退。
每分区超过64灯或无法证明边界有限时写 `UINT32_MAX`，对应片元遍历完整灯表。
范围0和方向灯始终入表；聚光灯使用range球，暂不做光锥细化。

分配覆盖完整视锥，不依赖不透明深度，MASK/BLEND可读取相同分区。
每个cluster由8个角的世界AABB做保守range球相交；坐标、深度与浮点边界已在决定中约定。
这会包含多余灯，尤其旋转视角及大range灯；首轮优先保证不会漏掉有贡献的灯。
不创建自动厨房灯光；emissive网格依然不是KHR_lights_punctual灯。
本增量不增加阴影或室内环境光遮蔽，M5接CSM/局部阴影，AO与局部反射仍在后续阶段。

## 资源与同步

- 每slot增加128-byte config（binding7、host visible）和独立indices（binding8、device local）。
  binding6灯SSBO同时可由compute/fragment读取。所有写入/扩容/该槽描述符更新均在其fence完成后。
- 原生1080p为30×17×24=12240 cells，indices payload为3,182,400 bytes/slot；
  禁用/零灯未增长时仅4-byte占位。资源只增长，禁用后保留已分配容量，退出通过账本释放。
- 同一图形/计算队列、一个持久compute pipeline，每workgroup64个invocation，一invocation一cell。
  compute写满每cell的计数及64个索引，避免未定义尾部内容。
- Render Graph新增buffer读写合同与RAW/WAR/WAW推导；编译产生
  ShaderStorageWrite/Compute → ShaderStorageRead/Fragment屏障。跨帧slot读写历史保留。
  compute读取再fragment读取时保留原写入者可见性，不丢失host/compute写入范围。
- 不增加runtime GPU等待/上传提交；pipeline初始计数在支持compute时12→13，resize仍增加12。
  帧SSBO共3个，fragment资源总预算21，材质16-sampler上限不变。

## 调试与测量

PBR debug `Cluster Light Count`：灰度=count/64，紫色=完整逐灯回退（包括溢出）；
关闭剔除也显示紫色。Render Debug显示当前网格、图中的compute pass及buffer屏障。
每slot timestamp pool增加queries10/11，分别记录culling起止；禁用帧只收取原10个queries。
原total/shadow/main/output/UI含义不变。报告schema4添加
`light_culling_requested`、`gpu_culling_ms`分位数及CSV `gpu_culling_ms,clustered_active`。
requested是启动请求；每帧active记录实际路径，不能从requested推断compute已运行。
GPU时间仅在提交完成后交付。软件时间不用于声称4060Ti收益或达标60FPS。

示例：

```bash
./run.sh --no-build --light-culling clustered assets/render_tests/punctual_reference.gltf
./run.sh --no-build --light-culling full assets/render_tests/punctual_reference.gltf
```

## 验证记录

Release/Ninja最终构建，22/22 CTests通过（15 CPU、7 llvmpipe/X11 GPU），228.77秒。
4条GPU事务路径共84组HDR完整图像对照；3条真实Application/UI异步测试也通过。
CPU grid/graph ASan+UBSan、compute/fragment `spirv-val --target-env vulkan1.3`通过；
SPIR-V bindings6/7/8、128-byte config各成员offset及工作组64已核对。
源码指纹为 `9b75c613447797a26354829ba36d7b914b88febededc45d701efda5840a3e065`（87项输入）。
所有软件测试显式限定lvp ICD和`MESA_VK_WSI_DEBUG=sw,noshm`，并取消WAYLAND_DISPLAY。
当前未启用validation layer；计数0不能代表验证层验收。原生WSI/4060Ti边界仍开放。

viewer预热3秒、测量2秒后共157个报告帧，frame/query一一对应、实际active符合模式、
prepared/retired/staging归零、呈现释放证明为true；仅作功能烟测，不作性能比较：

| 场景/模式 | 分辨率/在途帧 | UI/相机 | 报告帧 | 实际clustered |
|---|---|---|---:|---|
| punctual full | 1920×1080 / 2 | 关闭/static | 21 | 否 |
| punctual clustered | 1920×1080 / 2 | 关闭/static | 20 | 是 |
| punctual full | 640×360 / 1 | 关闭/static | 45 | 否 |
| punctual clustered | 640×360 / 1 | 开启/orbit | 37 | 是 |
| 原版厨房 | 640×360 / 1 | 开启/static | 4 | 否（零局部灯） |
| 剖切厨房 | 800×600 / 2 | 开启/static | 3 | 否（零局部灯） |
| Flight Helmet | 800×600 / 2 | 开启/static | 27 | 否（零局部灯） |

原生1080p两槽full/clustered的persistent payload差精确为6,364,792 bytes，
即2×(3,182,400−4)，对应两个indices增长；不等同driver VRAM或物理驻留。
聊天工作区`outputs/m4b/verification.json`、`ctest-last-test.log`、`source-manifest.txt`
和`warm-smokes/`保留原始报告与核对结果。

已检查CPU grid/fallback、独立几何采样、buffer合同和报告/启动开关。
生产GPU测试逐像素比较完整RGBA16F图，允许绝对误差0.003或相对0.7%较大者，
每条WSI配置21组full/clustered对照，涵盖0/1/16/32/64/65、真实有限灯剔除、
溢出、range0/方向/聚光、材质/法线AA、MASK/BLEND、移动相机/灯/物体和切片两侧。
GPU回读索引确认15盏远灯被剔除，并确认65灯触发overflow，不能通过总是fallback蒙混。
双pending槽65/1灯与相机z=3/4各自扩容，返回HDR .25/.75；所有查询交付一次、ID不回退。
资产候选、取消、退休、resize、释放证明及最终VMA/ledger归零仍由原回归覆盖。

## 下一项与开放边界

M5-A：稳定CSM，先规定相机/光方向、级联split、投影稳定、texel snapping、
边界混合及depth/normal bias，再实施和数值/运动回归。M5-B分配少量聚光灯阴影。
补充有显式灯光的室内阴影参考场景；厨房原版/剖切版继续作为资产与材质检查场景。
4060Ti硬件GPU成本/收益、RenderDoc、原生WSI、validation layer和运动画质仍待用户换机。
光锥细化、紧凑列表/全局灯表、自动低灯数阈值需要目标硬件数据再决定。
