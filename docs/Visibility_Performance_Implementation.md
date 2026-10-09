# 厨房可见性与性能增量

2026-10-08。实施依据见 [决定](Visibility_Performance_Decision.md)。
本轮减少不可见绘制；材质、灯光、曝光、阴影精度和渲染分辨率保持一致，没有新增画质效果。

## 修改

- 共享 Vulkan ZO 六平面/AABB helper，double 仅用于 CPU 平面测试；包含边界、抖动余量、
  数值误差余量及非法/未知 bounds 的保守回退。Application 默认相机视锥剔除开启。
- CSM/spot tile 独立筛选完整场景的投影者；主相机可见列表只用于主画面和级联接收面判断。
  完整实例继续用于运动历史、资源退休及探针身份。legacy 单图仍完整提交。
- 没有接收面覆盖的太阳级联跳过绘制，包含前一级的混合区间；不透明、mask 和 blend
  都计入接收面。未知 bounds 保留；shadow debug 和 probe capture 绕过级联接收面优化。
  atlas 仍全部清为远深度。普通 raw atlas 预览会显示未使用级联的清空结果；直接 shadow debug 保留图内容。
- 相机位置/允许状态独立保存在 CPU frame context；GPU buffer/image/descriptor/pipeline、
  shader 和 UBO/push ABI 均未增加或修改。
- 独立 `--camera-culling on|off`、`--shadow-culling on|off`，默认都开启；UI 保留调试开关及候选/剔除数。
  launcher 同时补齐原本缺少的 `--no-taa`/`--taa-history` 转发。
- 报告加入实际几何开关、`cpu_acquire_ms` 分布和 `slow_acquire_frame_count`（>100ms）；
  保留所有慢帧，不用筛选数据宣布达标。Render Debug 对长 acquire 给出窗口可见性提示。

## GPU 正确性

真实 GPU fixture 在实现前失败：开启 culling 仍保留全部候选；实现后 shadow draws 6→2、
主对象 3→1。离屏投影者仍影响可见接收面；未知包围盒保留；mask/blend 接收面和
用于调试的级联图通过 HDR 对照。

完整厨房在三个固定视角做 full/camera/shadow/combined 配对：九组线性 HDR RGB 最大差均为 **0**。
改变 culling 后重新捕获房间探针，前后完整输出最大差同样为 **0**。
图像对照使用无 TAA 的 Renderer 路径，避免不同采样相位污染数值比较；性能对照启用 TAA。

| 图像视角（942×1012） | 完整 Main/Shadow | 联合优化 Main/Shadow |
| --- | --- | --- |
| 默认室内 | 299 / 1758 | 207 / 1121 |
| 另一室内朝向 | 299 / 1758 | 48 / 1109 |
| 室外整体 | 299 / 1758 | 299 / 1125 |

原生 1920×1080 默认宽屏视角下，所有当前 AABB 仍与视锥相交，Main 仍是 299；
所以该视角的收益主要来自阴影，而非相机剔除。更少 draws 不代替真实耗时验证。

## 无抓帧性能对照

最终生产指纹 `8bc9cd0b6ef89765b3e8234619fc7b522223a3f2f912a6597704986baf261004`。RTX 4060 Ti，X11/NVIDIA/FIFO，原生 1080p、两帧槽、
相同默认厨房相机/灯光、TAA 开启、无 UI/RenderDoc。5 秒预热 + 10 秒测量，
同一个最终构建分别关闭/开启 shadow culling；camera culling 两组均开启。

| 项目 | 阴影优化关闭 | 联合默认设置 |
| --- | --- | --- |
| 完成帧 / 有效 GPU queries | 1260 | 1383 |
| GPU total 中位数 ms | 7.530 | 6.844 |
| Main 中位数 ms | 4.675 | 4.725 |
| Shadow 中位数 ms | 2.492 | 1.756 |
| GPU total p95 ms | 11.102 | 10.403 |
| Shadow draws | 1758 | 1121 |
| >100ms acquire 帧 | 0 | 0 |

阴影时间降低约 **29.5%**，整帧 GPU 中位数降低约 **9.1%**。Main 变化不作为收益。
查询数匹配、退出后无待完成 present fences，prepared/retired/staging 账本为零。
这些短测不替代 30 秒预热/120 秒稳态、多个场景与运动质量的最终验收。

第一轮 full 基准含 8 个慢 acquire 帧，已原样保留；没有拿它和正常运行比较以夸大收益。

## 呈现等待

同一小场景、同一进程，窗口可见→隐藏到另一个工作区→重新显示：
隐藏阶段出现 acquire 中位数 **995.712ms**，重新显示恢复普通刷新。
这定位了当前 Niri/XWayland/NVIDIA 路径下与窗口可见性相关的 ~1Hz 等待；
精确哪一层实施限速仍未单独源码追踪，不推断所有 WSI 问题都有同一原因。

在确认测试窗口可见的原生条件下，**34/34 CTests** 通过（19 CPU、15 GPU），总计 52.01 秒。
先前 generic legacy/EXT 的两个 120 秒超时用例分别 5.05/5.08 秒通过；单项 timeout 未增加。
该结果说明可见条件下同步/资源回归通过，不能把隐藏窗口的长等待当作固定算法成本。
legacy 释放证明、Wayland/原生最小化、validation layer 和最终画质/长测仍各自保留边界。

CPU helper ASan+UBSan 通过；当前沙箱的 LeakSanitizer 因 ptrace 不可用，运行时仅关闭了 leak 检查。
本轮无 GLSL 修改，未重复无关 SPIR-V 检查。临时诊断日志已移除。

## 使用与后续

```bash
./run.sh --no-build assets/models/pbr_kitchen/source/kitchen_core.gltf
# 明确的全绘制对照
./run.sh --no-build --camera-culling off --shadow-culling off assets/models/pbr_kitchen/source/kitchen_core.gltf
```

测量时保持测试窗口可见，检查报告中的 acquire 分布和慢帧计数；不要丢弃慢帧或通过锁时钟掩盖等待。
证据在本次聊天 `outputs/visibility-performance/`：窗口状态、native CTest、图像/原始 HDR、
最终配对报告和脚本均保留。下一项继续 M6-C 固定运动路径检查，然后 M7 GTAO；
本轮优化没有补上 AO、SSR、玻璃透射或各向异性 BRDF。
