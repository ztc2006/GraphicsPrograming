# RT-A 独立光追管线实施记录

2026-10-09。用户“推进光追管线”确认完整方案，实施独立Vulkan RT Pipeline＋SBT第一增量。已实现代码，最终原项目构建和验证状态在末尾补记。

## 可用入口和阶段边界

Render Debug → Rendering method → Raster / Ray tracing。CLI可用`--render-method raster|ray-tracing`，run.sh透传此参数。默认仍为Raster；非选中路径不执行场景pass，切换保持相同场景、相机、材质引用和曝光/显示输出，光栅历史重置。

此增量是RT主可见性/基础材质颜色预览。RT-B尚待共享完整光照、阴影、反射和多反弹GI；RT-C基础实体/薄片玻璃；RT-D成熟降噪库与单独透射处理。当前BLEND为固定像素coverage近似，miss为基础背景；当前画面不是完整物理路径追踪结果，也不用于助手主动评价画质。

## 实现合同

可选启用AS/RT Pipeline/deferred host operations、BDA/scalar/int64/非均匀采样索引，VMA只在启用BDA时设置对应flag。Device增加可明确禁用RT的能力配置用于回退验证，正式运行默认自动协商。缺少启用特性时保留光栅并给出原因。

共享原网格GPU存储并增加AS输入/BDA用途；112B Vertex按scalar读取。候选上传批次构建每mesh BLAS，与纹理/网格同一submit/fence提交；AS存储随scene作用域准备/提交/退休。BLAS scratch计入Staging临时域，上传完成后释放。每帧槽分别持有TLAS、实例输入、64B实例材质表、完整填充的固定512贴图descriptor数组（256材质）；场景超限只禁用RT模式，光栅仍能加载/运行，兼容场景恢复RT资格；当前每个RT帧重建TLAS，未实现UPDATE/静态复用优化。

独立RGBA32F HDR，原生分辨率下通过共用display输出。1920×1080额外RT HDR像素payload为33,177,600B；不等于全部RT显存，AS/SBT/实例表和构建峰值另计。原HDR16F、TAA/AO资源仍保留供切回光栅。RT不执行GTAO、TAA、cluster和阴影pass，不复用光栅历史。

RenderGraph增加RT storage-image full-write状态和RT→sampled转换；显式TLAS build → primary trace → display。AS输入/构建/读取、host表/SBT读内部依赖有明确barrier，scratch及SBT按设备alignment对齐。成功提交才发布RT输出状态。GPU main统计在RT模式中包含TLAS build＋primary trace，不是纯trace测量；CSV逐帧记录gpu_ray_tracing_active，summary记录selected HDR及main语义。

完整场景实例驱动RT，不使用相机剔除后的光栅列表；primaryVisible保持厨房4光卡不堵窗。Any-hit处理MASK、单双面与当前剔除/绕序设置。负缩放测试捕获重复面朝向翻转，已修正；法线数据未被整体翻转。原始厨房glTF/bin SHA匹配旧固定清单。

## 验证与证据

最终源码指纹：`1d077aebf8634ffacb85c04eb04348f8ce93d45c4c30224a49a0d86cbbae2f48`。GPU shader target明确Vulkan1.3；4个RT shader的spirv-val通过。解析GPU回读验证颜色、法线、独立ray/plane命中距离、MASK、隐藏实例、完整场景、刚体和镜像/面剔除。timeline gate真实阻塞GPU，同时提交不同帧槽颜色，证明table/descriptor互不覆盖；还检查RT↔Raster切换、失败保留、场景替换、输出重建及退出账本归零。

完整原生RTX/X11/KHR回归及1080p短测最终结果见full-suite-capacity.log、original-smoke.log和profile-original目录。旧账本按实际driver查询的BLAS尺寸精确核对，临时scratch与长期AS存储分域，保留新旧共存和取消/退出归零要求。先前38/42失败版本和镜像失败日志保留，没有放宽功能断言或原90/120秒用例时限。

厨房GPU基础预览导出raw RGBA32F和EV0、无tonemap的sRGB PNG；测试窗口实际942×1012，图像仅是工程预览，不标作正式1080p对照。正式1080p验证使用生产viewer benchmark，记录真实帧缓冲和模式/query匹配。数据不等于完整RT效果的帧预算/视觉验收。

当前llvmpipe也暴露AS/RT API，软件设备不能自动当作无RT设备；旧“预期拒绝”探测30秒未退出，不作为拒绝证据。软件光栅小场景烟测通过。无启用RT的回退改由明确禁用特性的真实Vulkan设备配置验证，检查未创建RT资源、模式拒绝、光栅仍可运行与退出归零。当前未发现可用Khronos validation layer；原生Wayland首次出现suboptimal，最终验收采用X11，不宣称已关闭Wayland/旧WSI问题。

用户保留画质判断；助手仅记录实现正确性和客观数据。没有commit/push。

## 下一步

RT-B从共享材质/灯光与直接光、可见性开始，再反射/多反弹GI及独立采样累积；明确光卡的主可见、阴影与发光角色，保留屏幕外/隐藏几何，避免重复叠加光栅probe SH/AO。玻璃与NRD按已确认RT-C/D推进，CPU对象BVH/Hi-Z/indirect/LOD和SDF继续后置。

## 最终交付核对

最终原生RTX4060Ti/X11/KHR回归 **43/43，170.29s**（20 CPU、23 GPU）；包括材质超限保留光栅和重新加载恢复RT。此前最终43项自动协商/禁用配置版本也全通过70.17s；最后一轮通用呈现用例变慢但均在原时限内完成，未筛除慢帧。原目录build-linux构建成功，源码指纹与staging一致；原目录六项烟测 **6/6，2.40s**。41文件及后续容量回退修正逐文件hash匹配；原版本备份、最终patch保留。

最终原目录生产viewer：原生1920×1080、2 slots、orbit、5s warmup/10s sample、1799个有效GPU查询/RT模式标签；GPU total中位数1.562880ms、RT main（TLAS build＋primary trace）1.498240ms，0 slow acquire、0 pending present与prepared/retired/staging资源。没有将这个基础pass成本当作完整GI/玻璃/降噪成本或60FPS验收。

中间相同代码9bf6构建失焦轮25帧/11 slow acquire；保持焦点复核1800帧/0 slow acquire，两轮原始数据保留。最终原目录1d077构建保持焦点1799帧/0 slow acquire。窗口焦点影响仍是呈现接受条件，不宣称修复了此前WSI根因。

未安装NRD；未实现RT-B/C/D。无commit/push，继续RT-B。用户保留视觉判断。
