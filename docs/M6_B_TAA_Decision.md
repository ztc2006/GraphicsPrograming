# M6-B：原生分辨率 TAA resolve

2026-10-07。已确认4060 Ti 8GB/615.71.09；设备访问需在Codex沙箱外执行。先验证已有路径，M6-A保留提交顺序和每槽运动输入。实时渲染advisor依据RTR4第6章采样/重建及[Yang等TAA survey](https://research.nvidia.com/labs/rtr/publication/yang2020survey/)。本阶段不引入超分辨率、GI或GTAO。

选用独立fullscreen fragment resolve，置于线性HDR与显示变换之间。复用现有Graph颜色附件/采样依赖；对比compute方案，本次避免扩充storage image状态和异步队列；MSAA单独不足以处理材质/镜面时间闪烁。颜色使用RGBA16F双缓冲，当前/前帧线性视深使用R32F双缓冲，1080p新增像素载荷49,766,400B。resolve自有descriptor layout/pipeline，不扩充主PBR fragment资源；描述符按读写奇偶预构建，双帧提交不在飞行中改descriptor。单队列保证上一帧history写→当前帧读/重用的RAW/WAR/WAW。

motion.w改存previousClip.w（标准透视投影的线性视深）；当前深度从D32和raster inverseVP重建同一度量，历史深度R32F。FP16前深度采用相对/像素深度梯度容差，避免旧FP16 device depth远处的量化误差；天空历史深度0且单独判类。geometry velocity保持未抖动UV；prevUV=currentUV-velocity+prevJitter-currentJitter。每帧无历史、motion有效度不足、越界、前深度不匹配/背景变化强制当前值。

历史颜色双线性重建，当前3x3邻域以YCoCg进行均值/方差及min/max限制，history限制到当前证据范围，移动时增加当前权重；基础current权重.1，快速移动可达.4。邻域深度/运动选择以靠近当前表面的深度为依据，禁止把前景速度盲目扩大到遮挡显露的背景。透明覆盖降低validity，先不积累；M6-C再细化reactivity/灯光和镜面响应。第一轮不额外锐化以免遮盖模糊问题。

reset统一失效相机/对象及颜色/深度历史：scene commit、camera cut/reset、resize、AA开关；成功submit才更新history parity/ready。capture不调用resolve/发布history；数据debug绕过TAA及jitter（切换时重置），UI始终显示变换后。TAA默认可开，通过UI和CLI可关用于同视角比较；render graph/timing/报告列出实际启用和独立resolve成本。

验收：精确输入GPU fixture证明静止收敛、正确UV符号/jitter补偿、物体运动、前深度/越界/透明拒绝、邻域clamp、明亮HDR/显示EV分离、reset和双pending槽。厨房/四合院/FlightHelmet原生1080p固定轨迹，对比off/on捕获画面和GPU时序；RenderDoc检查HDR→resolve→display顺序、history pingpong及运动/深度。RTX结果与软件回归分别标明；validation layer当前未安装，不把RenderDoc或通过测试称为validation通过。
