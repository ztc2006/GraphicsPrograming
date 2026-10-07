# M4-B：clustered forward 实施前决定

2026-10-05。采用同一图形/计算队列上的 GPU 光源分配，保留逐灯 forward 对照。
固定 64×64 屏幕分块、24 个对数视深度切片，每个 cluster 保存最多 64 个索引。
Linux/4060 Ti、原生1080p/60FPS、静态场景及可移动相机/物体/灯、透明支持等约束沿用。
本轮交付功能与同步正确性，不以 llvmpipe 耗时证明目标硬件性能。

## 比较与范围

- 逐灯：现有正确性参考，像素成本随灯数增长；始终可选。
- tiled：实现较小，但2D分块在深度跨度大的房间可能包含许多无关灯；仅用不透明深度
  缩减列表还会遗漏透明物体。暂不采用。
- clustered：屏幕XY+视深度分区，对完整视锥分配，覆盖透明表面；先使用保守世界AABB
  与点/聚光 range 球相交。聚光锥细化、紧凑列表、全局无限灯表等后续按硬件测量优化。
- CPU分配：可作独立数学验证，但不引入每帧CPU遍历全部cluster/灯的生产路径。

不增加深度预通道，不用法线剔除，不改变BRDF、曝光、太阳阴影或IBL。
局部灯阴影仍归M5，完整室内遮蔽还需要后续AO/反射工作。

## 坐标与精度

使用 Vulkan 正高度viewport的 framebuffer XY；gl_FragCoord和相同viewport计算tile。
从 inverse view-projection 的近/远中心和相机世界位置恢复视轴及正near/far。
仅接受可验证的标准透视投影；正交、奇异、非有限或不适用的投影回退逐灯。
切片 z=floor(log(depth/near)/log(far/near)×24)，边界clamp；视深度为
dot(worldPosition-camera,forward)。超出near/far的片元回退逐灯。
每个cluster由四角近面射线与两条深度平面构造8角世界AABB，添加保守浮点余量。
有限range使用球/AABB保守相交，聚光不做锥剔除；directional/range0必须纳入列表。
GPU产生非有限边界或每cluster超过64灯时写overflow标记，片元完整遍历全部灯，
不能静默截断。索引保持原灯序，比较不引入无必要的浮点求和乱序。

## 资源与同步

每个frame slot新增只读config与GPU写索引SSBO；set0 bindings7/8，binding6灯表同时
暴露给compute/fragment。槽fence完成后、acquire前更新参数与扩容并改该槽描述符；
其他pending槽保持原有资源。config与light host写入flush后由submit可见。
索引分配检查maxStorageBufferRange、32位索引、dispatch组数与compute队列/工作组
限制；不适用时保留逐灯，不让fallback读取未初始化列表。分辨率变化按slot安全扩容。

最小Render Graph加入非拥有的全buffer状态/用途，推导RAW/WAR/WAW依赖：
`Cluster light assignment`读取host灯/config、写indices，主通道读fragment灯/config/indices。
计划生成compute ShaderStorageWrite → fragment ShaderStorageRead屏障；旧slot的读写
历史同样进入下一次分配屏障。单队列，不增加semaphore、upload submit或runtime wait。
增加独立culling timestamp，保留原shadow/main/output/UI测量的含义。

## 验证合同

CPU：独立几何采样证明片元所属cell包含自身，tile/depth边界、反转Y、移动相机、宽FOV、
大near/far比、无效投影与资源上限fallback。图：buffer依赖/屏障/未定义读/重复资源/循环。
GPU：真实生产compute→fragment的HDR像素对照逐灯（0/1/16/32/64/65灯、range/spot/
方向/无限灯、溢出、屏幕与切片边界、相机/物体/灯移动、MASK/BLEND和环境/材质AA）。
至少一个有限灯不相交的case必须证明确实被剔除，而非始终fallback。两槽测试覆盖
pending独立资源、增长、参数变化、query唯一性与退出归零。保持既有资产上传/退休回归。
厨房、Flight Helmet、多光源参考做真实viewer烟测；GPU预算/运动画质/RenderDoc等4060Ti。

## 依据

RTR4 第20章（高效着色）与第9章（直接光）；不照搬完整引擎。
[Olsson/Billeter/Assarsson, 2012 原始论文](https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf)
提供分组光源分配思想，本实现不声称等同其全部优化或性能。
[Khronos 同步实例](https://docs.vulkan.org/guide/latest/synchronization_examples.html)
与 [Compute shader 指南](https://docs.vulkan.org/guide/latest/compute_shaders.html)
核对同队列storage写读依赖及工作组限制（2026-10-05）；这里只使用既有Vulkan能力。
