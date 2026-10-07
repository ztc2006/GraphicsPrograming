#pragma once
#include <filesystem>
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
void exerciseSpecularAa(Device const &device, SwapChain const &swapchain,
                        unsigned framesInFlight = 1);
void exercisePunctualLights(Device const &device, SwapChain const &swapchain,
                            unsigned framesInFlight = 1);

void exerciseSunCascades(Device const &, SwapChain const &, unsigned);

void exerciseIndoorLighting(Device const &, SwapChain const &, unsigned);

void exerciseKitchenScene(Device const &, SwapChain const &, unsigned, std::filesystem::path const &, std::filesystem::path const &, bool colourDiagnostic=false);

void exerciseTemporalMotion(Device const &, SwapChain const &, unsigned);

void exerciseTaaResolve(Device const &,SwapChain const &,unsigned);
