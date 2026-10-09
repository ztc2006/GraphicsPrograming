# 厨房可见性、反射与重复面修复决策

2026-10-08，用户已要求解决上一轮定位的三项问题。沿用diagnosing-bugs反馈循环与real-time-rendering-advisor，RTR4第4/9/11–12章变换、PBR/间接光与局部捕获原则；实际原资产/GPU对照见本聊天outputs/kitchen-geometry。RTX4060Ti、单graphics queue、1/2slots、1080p，原始资产保留。

## 选择与替代

全局翻法线/关剔除会破坏正常面，不能修复probe二次遮挡。隐藏所有emissive也会丢失真实灯具。选择显式PBRT area-light extras识别光卡角色，仅在查看器准备阶段使光卡对primary camera和shadow不可见；完整实例、probe捕获和退休/历史仍保留，普通emissive不变。不是按mesh编号/文件名硬编码隐藏。

真实RT/SSR引入加速结构或屏幕信息边界；强行提高粗糙度只掩盖错反射。选择当前单房间probe旁增加一个可选的局部镜面probe，用更小的包围区域覆盖微波炉内腔，capture point在内腔中。按点所在区域选择局部cube并作该区域box projection，室内SH仍沿用room，不宣称完整多probe混合/GI或RT。这是静态/按需捕获的有限修复；物体/灯/区域变化显示refresh required。kitchen明确坐标preset配置内腔，其他场景默认关闭；UI可检查设置。

重复面修复仅对可证明等价的原始triangle处理：删除零面积三角形；同primitive双面不透明、无纹理的同位置反向面，在对应法线/顶点色一致时去重。保留第一面/mesh/material/instance ID、边界和顶点数据，不合并不同材质/透明层/近共面，也不批量修复未知自交。原glTF/binary仍保留；该清理在查看器导入后CPU准备阶段，可复核统计。

## 数据与资源

SceneObject/ECS/DrawItem新增primaryVisible/shadowCaster语义。Renderer在main/shadow分别尊重，capture使用完整列表并保留光卡；caster语义进入probe几何身份。主UBO608B/push128B不变。

可选detail specular probe使用同一cube-array binding4：global层0、room层1、detail层2。IndoorLighting std430原1040B布局末尾追加detail min/max/position共48B（1088B/slot），flags bit4指detail有效；每slotfence后写入，不共享mutable配置。场景capture先drain，所有room/detail faces/bake/upload构造成功才原子发布cube array与region/key；失败保留旧probe，capture不推进常规帧history/query。普通frame不加pass/query，新增层约一个现有128cube/mip链载荷；capture增加6face与CPU预滤波，仅按需发生。默认1/2frame、graph共享图像同步和retirement保持。

## 验证闸门

新CPU seam先以no-op实现跑红：重复/零面积不减少，角色未生效；再实现并验证透明/单面/有纹理保守保留、ID稳定、重复prepare幂等。真实kitchen219zero-area/6反向重复必须导入后归零；原资产hash不变。

实际Renderer窗户same-camera修复前depth被光卡占据，修复后far/sky，完整probe仍包含card emission且光卡不投影；普通emissive仍可见。微波炉同view局部specular来源不再是room Floor，区域cube验证已知内腔第一遮挡，关闭detail作为失败control，reflection direction正常；room外输出保持既有probe。验证1/2slot独立detail SSBO、capture failure保留、refresh/scene change/reset/resize/ledger零、GPU SPIR-V/全量nativeCTests与fixed-view图像，独立短测记录新增frame成本，不以跨运行差宣称提速。
