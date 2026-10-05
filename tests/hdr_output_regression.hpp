#pragma once
class Device;
class SwapChain;
void exerciseHdrOutput(Device const &device);
void exerciseHdrScene(Device const &device, SwapChain const &swapchain,
                      unsigned framesInFlight = 1);
void exerciseMaterialContract(Device const &device, SwapChain const &swapchain,
                              unsigned framesInFlight = 1);
void exerciseEnvironmentIbl(Device const &device, SwapChain const &swapchain,
                            unsigned framesInFlight = 1);
void exerciseSpecularExtension(Device const &device, SwapChain const &swapchain,
                               unsigned framesInFlight = 1);
