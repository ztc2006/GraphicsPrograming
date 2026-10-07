# M6-C：清晰度与历史边界

2026-10-07，RTX4060Ti8GB、原生1080p60目标不变。M6-B已经有resolve/拒绝，但静止截图显示双线性历史重复重采样的柔化。本增量先交付可量化的重建与历史资格改进；移动灯光/材料/曝光响应一起做受控验证，原生acquire长等待不混入画质结论。

选择九次双线性读取的可分离Catmull-Rom历史重建，保留bilinear作为UI/CLI比较模式。中间两个正权重合并，两个负外瓣独立；CR结果先限制在可信历史足迹颜色范围，最后在当前YCoCg邻域证据内限制（允许.25sigma小幅边界余量，避免移动采样格的极值把未抖动细节反复削平），避免把锐化/振铃当细节。对比：提高current权重会损失时间积累，屏幕锐化无法恢复已经丢失的历史，B-spline更平滑；CR针对已有反复重采样原因，但代价更多读取且可能越界/跨表面。

先检查history坐标约定：M6-B保存raster-grid颜色并用previous-current jitter重采样，每帧静止画面也重新插值。M6-C改为固定的未抖动输出网格：未抖动velocity直接用于historyUV=outputUV-velocity，current jitter只改变当前采样，不移动history网格。用实际8相Halton静止正弦/64点积分reference量化，不能仅凭截图认定滤波原因；depth仍是当帧深度证据，坡度容差处理采样位置差。

CR最多4x4历史足迹逐tap检查深度及历史资格（只检查非零权重）；全部可信才使用CR。否则使用四tap深度/资格感知的双线性、归一化可信权重；可信覆盖太小就复制当前。天空单独0深度。此处不做前景速度扩张，避免遮挡显露背景借用前景运动。

修补透明历史资格：motion.z以+1表示有效previous、-1表示当前不透明但无可信previous，BLEND/debug输出0且继续同alpha blend。当前混合像素的abs(z)<1能显式标识不可信历史，包括完全不透明的BLEND。historyColor.a保存可积累资格，display仍输出alpha1。第一帧无previous(-1)保存可用当前颜色，但不读取旧历史；透明混合历史(a0)不能在它移开后被背景复用。清屏z0同样不积累。主PBR额外资源/attachments/UBO/push不增加。

filter参数借用TaaPush.jitterWeight.w；push仍112B，颜色/深度双缓冲保持，CPU模式变化失效颜色历史、不覆写飞行中图像。材质编辑/normal/POM开关在实际变化时清颜色历史；曝光作为display参数不清HDR历史。逐像素夹裁程度降低history confidence，响应明显的着色突变，同时防止把亚像素交替采样直接当光照变化。

验证：先在M6-B上构造失败用例，再比较半texel反复重采样的高频正弦对高采样解析reference的误差、静止交替收敛、移动边缘/前深度/UV边界、透明移开、无previous首帧、HDR常数/色彩/变化、两个pending提交。GPU fixture和actual viewer序列分别覆盖；厨房同机同视角bilinear/CR RenderDoc截图与通道时序，固定轨迹检查拖影及mask/specular细节。统计全部帧，不删除acquire慢帧宣称60FPS完成。

依据：RTR4第6章重建/采样与时间稳定；[GPU Gems2第20章](https://developer.nvidia.com/gpugems/gpugems2/part-iii-high-quality-rendering/chapter-20-fast-third-order-texture-filtering)讨论三次重建与合并线性读取（凸权重组合前提必须满足，CR负外瓣不能并入中间正项）；[TAA survey](https://research.nvidia.com/labs/rtr/publication/yang2020survey/)区分accumulation和history validation。本项目系数/阈值为工程选择，由上述数值测试证伪，不声称论文性能保证。

移动正弦判据：半像素平移的quarter-cycle信号，CR应至少减半双线性误差；解析CR插值不是完美带限重建，不能要求恢复全部高频。
