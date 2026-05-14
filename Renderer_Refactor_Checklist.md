# Renderer 重构清单

## 目标

这一阶段的目标是完成当前渲染器的生命周期重构，而不是继续叠加新的渲染功能。

只有满足下面这些条件，这一阶段才算完成：

- `Renderer` 的构造函数不再依赖 `SwapChain`
- 首次初始化和窗口 resize 后的重建都统一走 `renderer_->recreateForSwapChain(...)`
- `Renderer` 内部已经清楚分出“长期资源”和“依赖 swapchain 的资源”
- `drawFrame()` 已经按 `FrameContext + currentFrame_ + imagesInFlight_` 的模型组织
- `Application` 在 swapchain 重建时不再销毁并重建整个 `Renderer`

## 资源边界

### 长期资源

- `Device const &device_`
- `vk::raii::CommandPool commandPool_`
- `std::vector<FrameContext> frames_`
- `vk::raii::CommandBuffers commandBuffers_`
- `std::uint32_t currentFrame_`

### 依赖 swapchain 的资源

- `SwapChain const *swapChain_`
- `vk::raii::PipelineLayout pipelineLayout_`
- `vk::raii::Pipeline graphicsPipeline_`
- `std::vector<vk::ImageLayout> swapChainImageLayouts_`
- `std::vector<vk::Fence> imagesInFlight_`

## Renderer 接口改造

- 把构造函数改成 `explicit Renderer(Device const &device);`
- 把 `SwapChain const &swapChain_` 改成 `SwapChain const *swapChain_ = nullptr;`
- `FrameResult` 只保留这三种状态：
  - `eSuccess`
  - `eSwapChainOutOfDate`
  - `eSwapChainSuboptimal`
- 新增一个私有嵌套类型 `FrameContext`
- 新增这些函数：
  - `void recreateForSwapChain(SwapChain const &swapChain);`
  - `void createPersistentResources();`
  - `void createFrameResources();`
  - `void createSwapChainDependentResources();`
  - `void destroySwapChainDependentResources();`

## FrameContext 结构草案

```cpp
struct FrameContext {
  vk::raii::Semaphore imageAvailableSemaphore = nullptr;
  vk::raii::Semaphore renderFinishedSemaphore = nullptr;
  vk::raii::Fence inFlightFence = nullptr;
};
```

这里的约束是：

- `FrameContext` 作为 `Renderer` 的私有嵌套类型
- 不要把 command buffer 塞进 `FrameContext`
- `frames_[i]` 和 `commandBuffers_[i]` 永远表示同一个 frame slot

## 同步模型

- 这一阶段先固定 `kFramesInFlight = 1`
- command buffer 按 `kFramesInFlight` 分配，而不是按 swapchain image 数量分配
- 初始化时需要建立：
  - `frames_.resize(kFramesInFlight)`
  - `imagesInFlight_.assign(swapChain_->images().size(), vk::Fence{});`
  - `swapChainImageLayouts_.assign(swapChain_->images().size(), vk::ImageLayout::eUndefined);`

### drawFrame 规则

- 用 `currentFrame_` 选出当前 frame slot
- 在 acquire 前先等待当前 frame 的 fence
- 如果 acquire 阶段因为 out-of-date 失败：
  - 返回 `eSwapChainOutOfDate`
  - 不推进 `currentFrame_`
- 如果 `imagesInFlight_[imageIndex]` 不是空句柄，就先等它
- 用下面这句把 swapchain image 和当前帧关联起来：
  - `imagesInFlight_[imageIndex] = *frame.inFlightFence;`
- 只有在 acquire 成功之后、submit 之前，才 reset 当前帧 fence
- submit 时使用当前 frame 的 semaphore 和 fence
- 如果 present 发生 out-of-date 或 suboptimal，并且 submit 已经发生：
  - 返回对应的 `FrameResult`
  - 仍然推进 `currentFrame_`
- 只有在这次提交路径真的消耗了当前 frame slot 之后，才推进 `currentFrame_`

## 异常处理规则

- 在下面两个调用点周围捕获 `vk::OutOfDateKHRError`：
  - `acquireNextImage(...)`
  - `presentKHR(...)`
- `Renderer` 内部不要吞掉资源创建失败的异常
- `recreateForSwapChain(...)` 失败时，异常继续往上抛
- 顶层清理边界保留在 `Application::run()`

## recreateForSwapChain 的契约

把它实现成一次原子状态切换：

1. `destroySwapChainDependentResources()`
2. `swapChain_ = nullptr`
3. 绑定新的 swapchain
4. 创建新的依赖 swapchain 的资源

这里的规则是：

- `destroySwapChainDependentResources()` 不能去查询 `swapChain_`
- `destroySwapChainDependentResources()` 需要清掉：
  - `graphicsPipeline_`
  - `pipelineLayout_`
  - `swapChainImageLayouts_`
  - `imagesInFlight_`
  - `swapChain_`
- `createSwapChainDependentResources()` 必须要求 `swapChain_ != nullptr`
- 如果重建失败，要让 `Renderer` 留在“未就绪但干净”的状态，不能留下“半旧半新”的混合状态

## Application 侧改造

### initVulkan

- 用只依赖 `Device` 的方式创建 `Renderer`
- 首次初始化也必须走统一入口：
  - `renderer_ = std::make_unique<Renderer>(*device_);`
  - `renderer_->recreateForSwapChain(*swapChain_);`

### recreateSwapChain

- `device_->logicalDevice().waitIdle()` 继续留在 `Application`
- `SwapChain` 继续保留 `oldSwapchain` 交接逻辑
- 不再销毁并重建整个 `Renderer`
- 重建完 swapchain 之后调用：
  - `renderer_->recreateForSwapChain(*swapChain_);`

### mainLoop

- `eSwapChainOutOfDate` 和 `eSwapChainSuboptimal` 都立即触发重建
- `framebufferResized_` 继续作为额外的重建触发条件

## SwapChain 规则

- 保留 `oldSwapchain` 支持
- 第一次创建时不传 old swapchain
- 重建时传上一个原始 `vk::SwapchainKHR` 句柄
- 旧的 swapchain 对象必须活到新的 swapchain 创建成功之后

## 推荐实现顺序

1. 先改 `renderer.hpp`
2. 再改 `renderer.cpp` 里的资源归属和生命周期辅助函数
3. 再改 `renderer.cpp` 里的帧同步和 `drawFrame()`
4. 最后改 `application.cpp` 的初始化和重建流程
5. 用 `.\run.ps1 -Compiler clang` 重新编译验证
6. 对 resize 路径做压力测试

## 验收清单

- 程序仍然能正常绘制三角形
- 窗口 resize 正常
- 最小化和恢复正常
- 快速连续 resize 30 到 60 秒不崩溃
- resize / recreate 过程中没有 validation error
- `Application::recreateSwapChain()` 里不再整颗重建 `Renderer`
