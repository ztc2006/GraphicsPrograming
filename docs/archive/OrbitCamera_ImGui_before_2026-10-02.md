# Orbit 相机与 ImGui Docking 规划

## 目标

先做 orbit 相机控制，再接 ImGui docking。顺序不要反过来。

## 第一阶段：Orbit 相机控制

实现规则：
- 右键按住拖动，围绕 `Camera::target` 旋转
- 鼠标滚轮缩放
- 不做平移
- 不做 WASD
- 右键按住时隐藏并锁定光标
- 松开后恢复普通光标

结构边界：
- `Camera` 只保留位置、目标点、上方向和矩阵计算
- `OrbitCameraController` 单独保存 yaw/pitch/distance
- `Application` 只负责注册 GLFW 回调和每帧更新

每帧顺序：
1. `glfwPollEvents()`
2. `updateScene()`
3. `orbitCameraController.update(camera)`
4. 计算 `viewProj`
5. 传 `camera.position` 给 renderer

## 第二阶段：ImGui Docking

在相机控制稳定后再接入：
- 官方 docking 分支
- `imgui_impl_glfw`
- `imgui_impl_vulkan`
- DockSpace
- 相机面板
- 光照面板

第一版不做：
- 多视口
- 完整资源浏览器
- 大规模 renderer/material 重构

## 当前拆分原则

现在允许小封装，不做大重构：
- 可以把输入控制从 `Application` 拆出
- 可以把 ImGui 后续做成独立模块
- 先别拆 `MaterialSystem`，也别一次性重写 renderer 资源边界
