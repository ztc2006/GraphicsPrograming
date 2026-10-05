#pragma once
class Device;
class SwapChain;
class Renderer;
void exerciseFrameContexts(Device const &device, SwapChain const &swapchain);
void verifyDrainedFrameContexts(Renderer &renderer);
