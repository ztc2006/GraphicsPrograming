# M6-C：历史重建与透明边界

2026-10-07。依据见[M6-C选择](M6_C_TAA_Decision.md)。本轮实施清晰度、历史资格和局部响应；最终广泛运动画质及原生呈现接受仍开放。

## 已实现

历史现在固定在未抖动输出格，historyUV=outputUV-未抖动velocity。当前jitter改变本帧采样，不再推动历史网格。M6-B的raster-grid加jitter差约定已被替换；GPU静止8相Halton正弦和64点积分reference明确复现旧约定造成的持续模糊。

默认Catmull-Rom三次重建，九次双线性访问，中间同号正权重合并，外侧负瓣保留。所有有效4x4足迹tap必须匹配深度与history alpha资格，结果限制于可信历史颜色范围。跨表面时四tap感知双线性回退/可信权重归一化，覆盖不足复制当前；不使用前景速度扩张。邻域YCoCg统计保留，并给min/max .25sigma边界余量，防止抖动采样的极值每帧削平未抖动细节；variance限制与历史范围限制共同约束振铃。

motion.z改为+1有效previous、-1当前不透明但无previous、0透明/debug贡献；普通alpha混合后abs(z)<1显式表示混合资格。historyColor.a保存可复用资格，不再把透明混合颜色在透明物体移开后当背景历史。首帧-1复制当前并保存资格，下一帧仍可积累；清屏0不积累。显示输出alpha仍1。history sampler/resource/pipeline/目标像素载荷、UBO608B、主push128B及TAA push112B保持。

History reconstruction UI/--taa-history bilinear|catmull-rom可比较；模式变化只清颜色历史。实际材质tint/normal/POM/alpha编辑与表面开关变化清颜色，未变化的setter不会破坏积累。曝光仍是display参数，不清线性HDR历史。像素历史被邻域证据显著夹裁时增加current权重至.8，使明显的着色突变更快响应；不会仅因current/history的高频差异破坏亚像素收敛。

## 可证伪证据

旧shader新用例确实失败：透明移开后同深度背景读到混合历史；静止Halton正弦RMS=.159873（64点积分reference=.725085）。新约定RMS=.00148403。半像素移动正弦：bilinear=.630859、CR=.658203、连续reference=.676777，误差降到原约40.5%；CR并非完美带限重建，不要求恢复所有高频。

GPU fixture进一步证明跨深度足迹只取可信分量、首帧-1可播种而不读previous、透明/完全混合历史不复用、HDR常数保持、邻域矛盾快速响应、jitter不再移动history格、两timeline阻塞提交0/.1、重置与旧拒绝条件保留。Renderer实跑验证材质编辑/未变化setter、曝光、模式切换/未变化模式、normal/POM开关及独立TAA query。软件fixture与SPIR-V校验分开记录。

RTX全量初跑30/32通过（18CPU/12GPU）；legacy/EXT两个未启用TAA的generic WSI用例分别120s超时，单独复测仍超时。原始日志保留，不提高上限或宣布通过；KHR及独立历史用例通过。此前约997ms acquire等待仍是开放边界，原因尚未定论。VK_LAYER_KHRONOS_validation仍缺失，不称validation验收通过。

最终生产指纹`f23d10a7f4e8fd5e5351e10062587735a1b66af3c3456bde595ce81cd2fcc3a1`，103inputs；主要GPU SPIR-V Vulkan1.3校验通过。新增shader helper自动由.glSL依赖覆盖。软件fixture通过.24s。

RenderDoc导出同视角1080p厨房两种模式，各2084actions/29textures，HDR→resolve→display顺序保持。与M6-B同视角图对比，橱柜与抽油烟机边缘的持续柔化明显减少；截图只覆盖静止画质。4s暖机/8s短测：厨房CR769帧，TAA中位数.257024ms；新bilinear765帧，中位数.190464ms；四合院orbit1440帧中位数.217088ms，FlightHelmet orbit1440帧.37376ms。查询全部有效，prepared/retired/staging归零。厨房数据含capture注入，其余无注入；不将这些短测宣称为60FPS接受。短测/静止截图不等于整条运动稳定性接受；capture引入的时序与无注入数据分别标明，所有慢帧保留。

## 后续边界

继续固定场景/轨迹的mask、薄线、反射高光和透明细节审查；透明当前采用保守不积累，不承诺分层透明运动。动态GI、光追、超分与额外锐化未引入。原生呈现超时与质量调优分别推进，可靠后进入M7 GTAO。
