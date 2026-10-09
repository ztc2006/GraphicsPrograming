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
void exerciseVisibilityCulling(Device const &, SwapChain const &, unsigned);

void exerciseIndoorLighting(Device const &, SwapChain const &, unsigned);

void exerciseKitchenScene(Device const &, SwapChain const &, unsigned,
                          std::filesystem::path const &,
                          std::filesystem::path const &,
                          bool colourDiagnostic = false,
                          bool visibilityDiagnostic = false,
                          bool aoDiagnostic = false);

void exerciseTemporalMotion(Device const &, SwapChain const &, unsigned);

void exerciseGtao(Device const &, SwapChain const &, unsigned);
void exerciseTaaResolve(Device const &,SwapChain const &,unsigned);

void exerciseKitchenGeometry(Device const &, SwapChain const &, unsigned,
                             std::filesystem::path const &,
                             std::filesystem::path const &,
                             bool repaired = false);

void exerciseRayTracing(Device const &,SwapChain const &,unsigned);
void exerciseRayTracingKitchen(Device const &,SwapChain const &,unsigned,std::filesystem::path const &,std::filesystem::path const &);
void exerciseRayTracingDisabled(Device const &,SwapChain const &,unsigned);

void exerciseRayTransport(Device const &,SwapChain const &,unsigned);

void exerciseDielectricRT(Device const &,SwapChain const &,unsigned);
void exerciseDielectricRaster(Device const &,SwapChain const &,unsigned);
