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
## Renderer / SwapChain 重建补充

### 这一轮的目标

这一轮不是加新渲染功能，而是把 `Renderer` 和 `Application` 的 swapchain 重建路径收紧成事务式切换。

核心目标：

- `Renderer::recreateForSwapChain(...)` 提供 strong guarantee
- `Application::recreateSwapChain()` 也提供 strong guarantee
- 失败时不留下半旧半新的状态
- 成功时一次性提交新状态
- `drawFrame()` 只运行在“已提交且完整”的 renderer 状态上

---

## 设计原则

### 1. Renderer 重建必须是事务式的

`Renderer::recreateForSwapChain(SwapChain const &swapChain)` 不再走：

1. 先销毁旧资源
2. 再创建新资源

而是改成：

1. 校验传入的 `swapChain`
2. 在局部变量里创建新的 pipeline / semaphore / 状态数组
3. 全部成功后，用 `swap` 提交到成员
4. 最后再更新 `swapChain_`
5. 提交成功后把 `currentFrame_` 重置为 `0`

要求：

- 提交前不修改 `swapChain_`
- 提交前不修改旧的 `pipelineLayout_`
- 提交前不修改旧的 `graphicsPipeline_`
- 提交前不修改旧的 `renderFinishedSemaphores_`
- 提交前不修改旧的 `imagesInFlight_`
- 提交前不修改旧的 `swapChainImageLayouts_`

### 2. Application 重建也必须是事务式的

`Application::recreateSwapChain()` 不再先 `move` 走成员 `swapChain_`。

而是改成：

1. 等待 framebuffer 尺寸恢复为非零
2. `device_->logicalDevice().waitIdle()`
3. 借用 `swapChain_->handle()` 作为 `oldSwapChainHandle`
4. 在局部变量里创建 `newSwapChain`
5. 调用 `renderer_->recreateForSwapChain(*newSwapChain)`
6. 两者都成功后，再 `swapChain_.swap(newSwapChain)`
7. 成功提交后再把 `framebufferResized_ = false`

要求：

- 提交前不 `std::move(swapChain_)`
- 失败时保留旧的 `swapChain_`
- 失败时保留旧的 renderer 已提交状态
- `framebufferResized_` 只在成功提交后清零

---

## 需要修改的文件

- `src/renderer.hpp`
- `src/renderer.cpp`
- `src/application.cpp`

---

## renderer.hpp 需要调整的点

### 删掉这些旧接口

- `createSwapChainDependentResources()`
- `destroySwapChainDependentResources()`

### 新增这些私有接口

- `void validateSwapChainCandidate(SwapChain const &swapChain) const;`
- `void validateSwapChainState() const;`
- `vk::raii::Pipeline createGraphicsPipeline(SwapChain const &swapChain, vk::raii::PipelineLayout const &pipelineLayout) const;`

### 保留这些成员边界

长期资源：

- `Device const &device_`
- `vk::raii::CommandPool commandPool_`
- `std::vector<FrameContext> frames_`
- `vk::raii::CommandBuffers commandBuffers_`
- `std::uint32_t currentFrame_`

swapchain 相关资源：

- `SwapChain const *swapChain_`
- `vk::raii::PipelineLayout pipelineLayout_`
- `vk::raii::Pipeline graphicsPipeline_`
- `std::vector<vk::raii::Semaphore> renderFinishedSemaphores_`
- `std::vector<vk::ImageLayout> swapChainImageLayouts_`
- `std::vector<vk::Fence> imagesInFlight_`

---

## renderer.cpp 需要调整的点

### 1. `recreateForSwapChain(...)` 改成 strong guarantee

实现顺序固定为：

1. `validateSwapChainCandidate(swapChain)`
2. 局部创建 `newPipelineLayout`
3. 局部创建 `newGraphicsPipeline`
4. 局部创建 `newRenderFinishedSemaphores`
5. 局部创建 `newSwapChainImageLayouts`
6. 局部创建 `newImagesInFlight`
7. 用 `swap` 提交成员
8. 最后提交 `swapChain_`
9. `currentFrame_ = 0`

提交阶段优先使用：

- `swap(pipelineLayout_, newPipelineLayout)`
- `swap(graphicsPipeline_, newGraphicsPipeline)`
- `swap(renderFinishedSemaphores_, newRenderFinishedSemaphores)`
- `swap(swapChainImageLayouts_, newSwapChainImageLayouts)`
- `swap(imagesInFlight_, newImagesInFlight)`
- 最后再提交 `swapChain_`

### 2. `createGraphicsPipeline(...)` 改成纯构建函数

要求：

- 不再读取成员 `swapChain_`
- 不再写成员 `pipelineLayout_`
- 只依赖传入的 `SwapChain const &swapChain`
- 只依赖传入的 `PipelineLayout`
- 返回新建好的 `vk::raii::Pipeline`

### 3. `drawFrame()` 开头先做状态校验

在任何操作前先调用：

- `validateSwapChainState()`

然后再访问：

- `frames_[currentFrame_]`
- `commandBuffers_[currentFrame_]`
- `swapChain_->handle()`

### 4. `drawFrame()` 里增加边界检查

至少显式检查：

- `currentFrame_ < frames_.size()`
- `imageIndex < renderFinishedSemaphores_.size()`

### 5. `validateSwapChainCandidate(...)` 检查这些条件

- `swapChain.images().size() > 0`
- `swapChain.imageViews().size() == swapChain.images().size()`
- `swapChain.imageFormat() != vk::Format::eUndefined`
- `swapChain.extent().width > 0`
- `swapChain.extent().height > 0`

### 6. `validateSwapChainState()` 检查这些条件

- `swapChain_ != nullptr`
- `frames_.size() == kFramesInFlight`
- `commandBuffers_.size() == kFramesInFlight`
- `currentFrame_ < frames_.size()`
- `pipelineLayout_` 已有效
- `graphicsPipeline_` 已有效
- `swapChain_->images().size() > 0`
- `swapChain_->imageViews().size() == swapChain_->images().size()`
- `renderFinishedSemaphores_.size() == swapChain_->images().size()`
- `imagesInFlight_.size() == swapChain_->images().size()`
- `swapChainImageLayouts_.size() == swapChain_->images().size()`

---

## application.cpp 需要调整的点

### 1. `mainLoop()` 不要在调用重建前提前清空 `framebufferResized_`

现在的要求是：

- 只有 `recreateSwapChain()` 成功提交后
- 才把 `framebufferResized_ = false`

### 2. `recreateSwapChain()` 开头加顶层状态检查

至少检查：

- `window_ != nullptr`
- `device_ != nullptr`
- `renderer_ != nullptr`
- `swapChain_ != nullptr`

### 3. `recreateSwapChain()` 改成 strong guarantee

推荐顺序：

1. 检查依赖成员存在
2. 等待 framebuffer 尺寸非零
3. `device_->logicalDevice().waitIdle()`
4. 读取 `vk::SwapchainKHR oldSwapChainHandle = *swapChain_->handle();`
5. 局部创建 `auto newSwapChain = std::make_unique<SwapChain>(...)`
6. `renderer_->recreateForSwapChain(*newSwapChain)`
7. `swapChain_.swap(newSwapChain)`
8. `framebufferResized_ = false`

### 4. 首次初始化保持现状

`initVulkan()` 仍然用：

- `renderer_ = std::make_unique<Renderer>(*device_);`
- `renderer_->recreateForSwapChain(*swapChain_);`

如果这里失败，直接让异常往上抛。

---

## 额外实现细节

### 头文件

如果实现里用了 `std::move` 或 `swap`，记得补：

- `#include <utility>`

### 提交阶段规则

提交阶段应该只做这些低风险动作：

- `swap`
- 指针切换
- `currentFrame_ = 0`

不要在提交阶段里再创建 Vulkan 资源。

---

## 这一轮不做的事

这轮不要顺手加入：

- vertex buffer
- index buffer
- staging upload
- push constant
- scene / entity / camera
- descriptor / material / resource manager

这轮只收口：

- swapchain 重建原子性
- renderer 内部状态完整性
- resize / recreate 异常安全

---

## 验收标准补充

编译标准：

- `.\run.ps1 -Compiler clang` 重新通过

运行标准：

- 三角形仍然正常绘制
- window resize 正常
- minimize / restore 正常
- 快速连续 resize 30 到 60 秒不崩溃
- resize / recreate 过程中没有 validation warning / error

语义标准：

- `Renderer::recreateForSwapChain(...)` 失败时保持旧状态不变
- `Application::recreateSwapChain()` 失败时保持旧状态不变
- `drawFrame()` 不会看到半更新状态
