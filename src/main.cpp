#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
constexpr std::uint32_t kWindowWidth = 1280;
constexpr std::uint32_t kWindowHeight = 720;
constexpr char kWindowTitle[] = "GraphicsPrograming";

class VulkanApp
{
public:
    void run()
    {
        initWindow();
        initVulkan();
        mainLoop();
        cleanup();
    }

private:
    GLFWwindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    void initWindow()
    {
        if (glfwInit() != GLFW_TRUE)
        {
            throw std::runtime_error("Failed to initialize GLFW.");
        }

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        window_ = glfwCreateWindow(
            static_cast<int>(kWindowWidth),
            static_cast<int>(kWindowHeight),
            kWindowTitle,
            nullptr,
            nullptr
        );

        if (window_ == nullptr)
        {
            throw std::runtime_error("Failed to create GLFW window.");
        }
    }

    void initVulkan()
    {
        createInstance();
        createSurface();
    }

    void createInstance()
    {
        std::uint32_t apiVersion = VK_API_VERSION_1_0;
        vkEnumerateInstanceVersion(&apiVersion);

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = kWindowTitle;
        appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        appInfo.pEngineName = "LearningEngine";
        appInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        appInfo.apiVersion = apiVersion;

        std::uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        if (glfwExtensions == nullptr || glfwExtensionCount == 0)
        {
            throw std::runtime_error("GLFW did not report Vulkan instance extensions.");
        }

        std::vector<const char*> requiredExtensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(requiredExtensions.size());
        createInfo.ppEnabledExtensionNames = requiredExtensions.data();

        const VkResult result = vkCreateInstance(&createInfo, nullptr, &instance_);
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create Vulkan instance.");
        }

        std::cout
            << "Vulkan instance created. API version "
            << VK_API_VERSION_MAJOR(apiVersion) << '.'
            << VK_API_VERSION_MINOR(apiVersion) << '.'
            << VK_API_VERSION_PATCH(apiVersion) << '\n';
    }

    void createSurface()
    {
        const VkResult result = glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
        if (result != VK_SUCCESS)
        {
            throw std::runtime_error("Failed to create window surface.");
        }
    }

    void mainLoop()
    {
        while (glfwWindowShouldClose(window_) == GLFW_FALSE)
        {
            glfwPollEvents();
        }
    }

    void cleanup()
    {
        if (surface_ != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }

        if (instance_ != VK_NULL_HANDLE)
        {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }

        if (window_ != nullptr)
        {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }

        glfwTerminate();
    }
};
} // namespace

int main()
{
    try
    {
        VulkanApp app;
        app.run();
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
