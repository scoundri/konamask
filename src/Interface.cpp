// most initialization code was taken from: https://github.com/ocornut/imgui/

#include "Interface.h"
#include "TextToSpeech.h" // for manual voice output
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <imgui.h>
#include <imgui_internal.h>
#include "Settings.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl2.h"
#include <linux/limits.h>
#include <stdlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <thread>
#include <vulkan/vk_platform.h>
#include <vulkan/vulkan_core.h>
#include <array> // for ImVec4 conversion
#include <limits.h> // for config.ini path
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h> // for texture (image) loading
#include <fstream> // for CheckFile and CopyFile
#include <sys/stat.h> // for CheckFile and CopyFile
#include <filesystem>
#include <vector>
#include <chrono>
#include <algorithm>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#define VOLK_IMPLEMENTATION
#include <volk.h>
#endif

//#define _DEBUG
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
static VkDebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
#endif

static VkAllocationCallbacks*   g_Allocator = nullptr;
static VkInstance               g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice         g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice                 g_Device = VK_NULL_HANDLE;
static uint32_t                 g_QueueFamily = (uint32_t)-1;
static VkQueue                  g_Queue = VK_NULL_HANDLE;       
static VkPipelineCache          g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool         g_DescriptorPool = VK_NULL_HANDLE;
static VkCommandPool            g_CommandPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
SDL_Window*                     window;
std::atomic<bool>               g_RenderPaused{false};
static uint32_t                 g_MinImageCount = 2;
static bool                     g_SwapChainRebuild = false;

// TextureData struct code from https://github.com/ocornut/imgui/wiki/Image-Loading-and-Displaying-Examples
struct TextureData {
    VkDescriptorSet DS;         // descriptor set
    int             width;
    int             height;
    int             Channels;

    // for proper cleanup
    VkImageView     ImageView;
    VkImage         Image;
    VkDeviceMemory  ImageMemory;
    VkSampler       Sampler;
    VkBuffer        UploadBuffer;
    VkDeviceMemory  UploadBufferMemory;

    TextureData() { memset(this, 0, sizeof(*this)); }
};

struct ImGuiFilePicker {
    
    std::filesystem::path current_path = std::filesystem::current_path();
    std::vector<std::string> image_exts = {".png", ".jpg", ".jpeg", ".bmp", ".gif"}; // .webp usually cause an exception
    std::string selected_path;
    std::string filter; // filter string (e.g. "*.png;*.jpg") - not implemented, reserved
    std::chrono::steady_clock::time_point last_click_time = std::chrono::steady_clock::now(); // last click time to detect double-click
    std::string last_clicked_item;

    // draw the widget > returns true when the user double-clicks a file (selection made)
    bool Draw(const char* title, bool* p_open, std::string* out_selected_path = nullptr) {
        if (p_open && !*p_open) return false;

        bool selection_made = false;

        ImGui::SetNextWindowSize(ImVec2(640, 360), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, p_open)) {
            ImGui::End();
            return false;
        }

        // path breadcrumb
        DrawBreadcrumb();

        ImGui::Separator();

        // two columns > directories on left, files on right
        ImGui::Columns(2, "filepicker_cols", true);
        DrawDirectoriesColumn();
        ImGui::NextColumn();
        DrawFilesColumn(&selection_made);
        ImGui::Columns(1);

        ImGui::End();

        if (selection_made && out_selected_path) {
            *out_selected_path = selected_path;
        }
        return selection_made;
    }
private:
    void DrawBreadcrumb() {
        std::string accum;
        ImGui::TextDisabled("current:"); ImGui::SameLine();
        // build clickable path pieces
        for (auto it = current_path.begin(); it != current_path.end(); ++it) {
            if (it != current_path.begin()) ImGui::SameLine();
            std::string part = it->string();
            if (ImGui::Button(part.c_str())) {
                // assemble path up to this element
                std::filesystem::path p;
                for (auto jt = current_path.begin(); jt != it; ++jt) p /= *jt;
                p /= *it;
                if (std::filesystem::exists(p) && std::filesystem::is_directory(p)) current_path = p;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("..")) {
            if (current_path.has_parent_path()) current_path = current_path.parent_path();
        }
    }

    void DrawDirectoriesColumn() {
        ImGui::Text("folders");

        // enumerate directories
        std::error_code ec;
        std::vector<std::filesystem::directory_entry> dirs;
        for (auto &de : std::filesystem::directory_iterator(current_path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (de.is_directory(ec)) dirs.push_back(de);
        }
        // sort by name
        std::sort(dirs.begin(), dirs.end(), [](auto &a, auto &b){ return a.path().filename().string() < b.path().filename().string(); });

        // display
        ImGui::BeginChild("##dirs", ImVec2(0,0), false, ImGuiWindowFlags_HorizontalScrollbar);
        for (auto &d : dirs) {
            std::string name = d.path().filename().string();
            if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    // enter directory on double click
                    current_path /= name;
                }
            }
        }
        ImGui::EndChild();
    }

    void DrawFilesColumn(bool* selection_made) {
        ImGui::Text("files");
        ImGui::Separator();
        std::error_code ec;
        std::vector<std::filesystem::directory_entry> files;
        for (auto &de : std::filesystem::directory_iterator(current_path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            if (de.is_regular_file(ec)) {
                auto ext = de.path().extension().string();
                // lowercase ext
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
                if (std::find(image_exts.begin(), image_exts.end(), ext) != image_exts.end()) files.push_back(de);
            }
        }

        std::sort(files.begin(), files.end(), [](auto &a, auto &b){ return a.path().filename().string() < b.path().filename().string(); });

        ImGui::BeginChild("##files", ImVec2(0,0), false, ImGuiWindowFlags_HorizontalScrollbar);
        for (auto &f : files) {
            std::string name = f.path().filename().string();
            bool selected = (selected_path == f.path().string());
            if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                // single click selects
                selected_path = f.path().string();

                // detect double click (using ImGui builtin)
                if (ImGui::IsMouseDoubleClicked(0)) {
                    // choose file
                    if (std::find(image_exts.begin(), image_exts.end(), f.path().extension().string()) != image_exts.end()) {
                        selected_path = f.path().string();
                        if (selection_made) *selection_made = true;
                    }
                }
            }
        }
        ImGui::EndChild();
    }
};



static void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS) 
        return;
    fprintf(stderr, "[ERROR] (Vulkan) VkResult = %d\n", err);
    if (err < 0)
        abort();
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_report(VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType, uint64_t object, size_t location, int32_t messageCode, const char* pLayerPrefix, const char* pMessage, void* pUserData) {
    (void)flags; (void)object; (void)location; (void)messageCode; (void)pUserData; (void)pLayerPrefix;
    fprintf(stderr, "[INFO] (Vulkan) Debug report from ObjectType: %i\nMessage: %s\n\n", objectType, pMessage);
    return VK_FALSE;
}
#endif

static bool CheckFile(const char* path) {
    struct stat info;
    if (stat(path, &info) != 0) {
        std::cerr << "[ERROR] File check failed: " << strerror(errno) << "\n[INFO] Path \"" << path << "\" does not exist." << std::endl;
        return false;
    }

    if (info.st_mode & S_IFREG) {
        return true; // path exists & is a file
    } else {
        std::cerr << "[ERROR] Path \"" << path << "\" is not a file." << std::endl;
        return false;
    }
}

static bool CopyFile(const std::string& src, const std::string& dest) {
    std::ifstream sourceFile(src, std::ios::binary);
    if (!sourceFile.is_open()) {
        std::cerr << "[ERROR] Could not open source file (" << dest << ")." << std::endl;
        return false;
    }

    std::ofstream destinationFile(dest, std::ios::binary);
    if (!destinationFile.is_open()) {
        std::cerr << "[ERROR] Could not open destination file (" << dest << ")." << std::endl;
        sourceFile.close();
        return false;
    }

    destinationFile << sourceFile.rdbuf();

    sourceFile.close();
    destinationFile.close();

    return true;
}

// vulkan helpers
uint32_t findMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0xFFFFFFFF;
}

VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool commandPool) {
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void endSingleTimeCommands(VkDevice device, VkCommandPool commandPool, VkQueue queue, VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
}

void transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkFormat format,
                           VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    barrier.srcAccessMask = 0; // TODO: set based on oldLayout
    barrier.dstAccessMask = 0; // TODO: set based on newLayout

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        throw std::invalid_argument("Unsupported layout transition!");
    }

    vkCmdPipelineBarrier(
        cmd,
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

void createBuffer(VkDevice device, VkPhysicalDevice physDevice, VkDeviceSize size,
                  VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                  VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer!");
    }

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = findMemoryType(physDevice, memRequirements.memoryTypeBits, properties);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory!");
    }

    vkBindBufferMemory(device, buffer, bufferMemory, 0);
}
//
static void CreateCommandPool() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
                              | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = g_QueueFamily;   // same family as your graphics queue

    if (vkCreateCommandPool(g_Device, &poolInfo, nullptr, &g_CommandPool) != VK_SUCCESS)
        throw std::runtime_error("failed to create command pool!");
}

static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension) {
    for (const VkExtensionProperties& p : properties) {\
        if (strcmp(p.extensionName, extension)==0) 
            return true;
    }
    return false;
}

static void VkSetup(ImVector<const char*> instance_extensions) {
    VkResult err;
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
volkInitialize();
#endif
    {  
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);        
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        check_vk_result(err);
    
        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char* layers[] = {"VK_LAYER_KRONOS_validation"};
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif

        create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        check_vk_result(err);

#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
        volkLoadInstance(g_Instance);
#endif

#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkCreateDebugReportCallbackEXT = (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = debug_report;
        debug_report_ci.pUserData = nullptr;
        err = f_vkCreateDebugReportCallbackEXT(g_Instance, &debug_report_ci, g_Allocator, &g_DebugReport);
        check_vk_result(err);
#endif

    }

    // select physical device
    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

    // select graphics queue family
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);
    // create logical device
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        // enumerate physical device extension
        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, properties.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
        check_vk_result(err);
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
        CreateCommandPool();
        }

    // descriptor pool
    {
        VkDescriptorPoolSize pool_sizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE },
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 0;
        for (VkDescriptorPoolSize& pool_size : pool_sizes)
        pool_info.maxSets += pool_size.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        check_vk_result(err);
    }
}

static void SetupVkWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height) {
    wd->Surface = surface;

    // check for WSI support
    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE) {
        fprintf(stderr, "[ERROR] (Vulkan) No WSI support on physical device 0\n");
        exit(-1);
    }

    // select surface format
    const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    // select present mode
#ifdef APP_USE_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
#else
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, &present_modes[0], IM_ARRAYSIZE(present_modes));
    //printf("[INFO] (Vulkan) Selected PresentMode = %d\n", wd->PresentMode);

    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount);
}

static void VkCleanup() {
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    // remove the debug report callback
    auto f_vkDestroyDebugReportCallbackEXT = (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(g_Instance, "vkDestroyDebugReportCallbackEXT");
    f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif

    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVkWindow() {
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);    // wait indefinitely instead of periodically checking
        check_vk_result(err);

        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    // record imgui primitives into command buffer
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    // submit command buffer
    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild)
        return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        g_SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

// create a VkSampler
VkSampler CreateFontSampler(VkDevice device) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    info.maxAnisotropy = 1.0f;
    VkSampler sampler;
    vkCreateSampler(device, &info, nullptr, &sampler);
    return sampler;
}

// create a VkImage + allocate & bind memory
VkImage CreateFontImage(VkDevice device, VkPhysicalDevice phys, int w, int h, VkDeviceMemory& outMemory) {
    VkImageCreateInfo imgInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imgInfo.imageType   = VK_IMAGE_TYPE_2D;
    imgInfo.format      = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent      = { (uint32_t)w, (uint32_t)h, 1 };
    imgInfo.mipLevels   = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage       = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image;
    vkCreateImage(device, &imgInfo, nullptr, &image);

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = req.size;
    // findMemoryType is your helper to pick a memory type index with VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    allocInfo.memoryTypeIndex = findMemoryType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    vkAllocateMemory(device, &allocInfo, nullptr, &outMemory);
    vkBindImageMemory(device, image, outMemory, 0);
    return image;
}

// create a VkImageView
VkImageView CreateFontImageView(VkDevice device, VkImage image) {
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format   = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,1, 0,1 };
    VkImageView view;
    vkCreateImageView(device, &viewInfo, nullptr, &view);
    return view;
}

void UploadFontPixels(VkDevice device, VkPhysicalDevice physDevice,
                      VkCommandPool cmdPool, VkQueue queue,
                      VkImage image, unsigned char* pixels, int width, int height) {
    // create a staging buffer
    VkDeviceSize imageSize = uint64_t(width) * height * 4; // RGBA8
    
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    createBuffer(device, physDevice,
                 imageSize,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingMemory);

    // copy pixel data into the buffer
    void* mapped;
    vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
    memcpy(mapped, pixels, size_t(imageSize));
    vkUnmapMemory(device, stagingMemory);

    // record commands to transition image and copy buffer→image
    VkCommandBuffer cmd = beginSingleTimeCommands(device, cmdPool);

    //undef > transfer‐dst
    transitionImageLayout(cmd, image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // copy
    VkBufferImageCopy region{};
    region.bufferOffset                    = 0;
    region.bufferRowLength                 = 0;
    region.bufferImageHeight               = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset                     = { 0, 0, 0 };
    region.imageExtent                     = { uint32_t(width), uint32_t(height), 1 };
    vkCmdCopyBufferToImage(cmd,
                           stagingBuffer,
                           image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &region);

    // transfer‐dst > shader‐read
    transitionImageLayout(cmd, image, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    endSingleTimeCommands(device, cmdPool, queue, cmd);

    // cleanup staging resources
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);
}

std::array<float,4> ImVec4ToFloats(const ImVec4 &c) {
    return { c.x, c.y, c.z, c.w };
}
inline ImVec4 FloatsToImVec4(const std::array<float,4>& a) noexcept {
    return ImVec4(a[0], a[1], a[2], a[3]);
}

static int Shutdown(VkSurfaceKHR g_Surface) {

        // wait for device idle if available
        if (g_Device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(g_Device);
        }

        // shutdown ImGui renderer & platform backends (renderer first)
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL2_Shutdown();

        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::DestroyContext();
        }

#ifdef IMGUI_IMPL_VULKANH_HEADERS_AVAILABLE
        if (g_Instance != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
            ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
        }
#endif

        // destroy Vulkan objects
        if (g_CommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(g_Device, g_CommandPool, g_Allocator);
            g_CommandPool = VK_NULL_HANDLE;
        }

        if (g_DescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
            g_DescriptorPool = VK_NULL_HANDLE;
        }

        if (g_PipelineCache != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(g_Device, g_PipelineCache, g_Allocator);
            g_PipelineCache = VK_NULL_HANDLE;
        }

        if (g_Device != VK_NULL_HANDLE) {
            vkDestroyDevice(g_Device,g_Allocator);
            g_Device = VK_NULL_HANDLE;
        }

        if (g_Instance != VK_NULL_HANDLE) {
            vkDestroyInstance(g_Instance, g_Allocator);
            g_Instance = VK_NULL_HANDLE;
        }

        if (g_Surface != VK_NULL_HANDLE && g_Instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(g_Instance, g_Surface, g_Allocator);
            g_Surface = VK_NULL_HANDLE;
        }

        // clear globals
        g_PhysicalDevice = VK_NULL_HANDLE;
        g_Queue = VK_NULL_HANDLE;
        g_QueueFamily = (uint32_t)-1;
        g_MinImageCount = 2;
        g_SwapChainRebuild = false;

        if (window) {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
        SDL_Quit();
        return 0;
}

void Interface::Minimize() {
    if (!window) return;
    // stop rendering loop
    g_RenderPaused.store(true, std::memory_order_release);

    // hide or minimize the window
    SDL_HideWindow(window);
    //SDL_MinimizeWindow(g_Window); // minimize to taskbar - scrapped (will not work on some WMs)
}

void Interface::Show() {
    if (!window) return;

    // show and bring front
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);
    SDL_RestoreWindow(window);

    // may need to recreate swapchain if window size changed while hidden

    g_RenderPaused.store(false, std::memory_order_release);
}

uint32_t findTextureMemoryType(uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties mem_properties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &mem_properties);

    for (uint32_t i = 0; i < mem_properties.memoryTypeCount; i++)
        if ((type_filter & (1 << i)) && (mem_properties.memoryTypes[i].propertyFlags & properties) == properties)
            return i;

    return 0xFFFFFFFF; // unable to find memoryType
}

// LoadTextureFromFile code from: https://github.com/ocornut/imgui/wiki/Image-Loading-and-Displaying-Examples
static bool LoadTextureFromFile(const char* filename, TextureData* tex_data) {
    // specifying 4 channels forces stb to load the image in RGBA which is an easy format for Vulkan
    tex_data->Channels = 4;
    unsigned char* image_data = stbi_load(filename, &tex_data->width, &tex_data->height, 0, tex_data->Channels);

    if (image_data == NULL)
        return false;

    // calculate allocation size (in number of bytes)
    size_t image_size = tex_data->width * tex_data->height * tex_data->Channels;

    VkResult err;

    // Create the Vulkan image.
    {
        VkImageCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent.width = tex_data->width;
        info.extent.height = tex_data->height;
        info.extent.depth = 1;
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        err = vkCreateImage(g_Device, &info, g_Allocator, &tex_data->Image);
        check_vk_result(err);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g_Device, tex_data->Image, &req);
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = findTextureMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        err = vkAllocateMemory(g_Device, &alloc_info, g_Allocator, &tex_data->ImageMemory);
        check_vk_result(err);
        err = vkBindImageMemory(g_Device, tex_data->Image, tex_data->ImageMemory, 0);
        check_vk_result(err);
    }

    // Create the Image View
    {
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = tex_data->Image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;
        err = vkCreateImageView(g_Device, &info, g_Allocator, &tex_data->ImageView);
        check_vk_result(err);
    }

    // Create Sampler
    {
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT; // outside image bounds just use border color
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.minLod = -1000;
        sampler_info.maxLod = 1000;
        sampler_info.maxAnisotropy = 1.0f;
        err = vkCreateSampler(g_Device, &sampler_info, g_Allocator, &tex_data->Sampler);
        check_vk_result(err);
    }

    // Create Descriptor Set using ImGUI's implementation
    tex_data->DS = ImGui_ImplVulkan_AddTexture(tex_data->Sampler, tex_data->ImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Create Upload Buffer
    {
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = image_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        err = vkCreateBuffer(g_Device, &buffer_info, g_Allocator, &tex_data->UploadBuffer);
        check_vk_result(err);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_Device, tex_data->UploadBuffer, &req);
        VkMemoryAllocateInfo alloc_info = {};
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.allocationSize = req.size;
        alloc_info.memoryTypeIndex = findTextureMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        err = vkAllocateMemory(g_Device, &alloc_info, g_Allocator, &tex_data->UploadBufferMemory);
        check_vk_result(err);
        err = vkBindBufferMemory(g_Device, tex_data->UploadBuffer, tex_data->UploadBufferMemory, 0);
        check_vk_result(err);
    }

    // Upload to Buffer:
    {
        void* map = NULL;
        err = vkMapMemory(g_Device, tex_data->UploadBufferMemory, 0, image_size, 0, &map);
        check_vk_result(err);
        memcpy(map, image_data, image_size);
        VkMappedMemoryRange range[1] = {};
        range[0].sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range[0].memory = tex_data->UploadBufferMemory;
        range[0].size = image_size;
        err = vkFlushMappedMemoryRanges(g_Device, 1, range);
        check_vk_result(err);
        vkUnmapMemory(g_Device, tex_data->UploadBufferMemory);
    }

    // Release image memory using stb
    stbi_image_free(image_data);

    // Create a command buffer that will perform following steps when hit in the command queue.
    // TODO: this works in the example, but may need input if this is an acceptable way to access the pool/create the command buffer.
    VkCommandPool command_pool = g_MainWindowData.Frames[g_MainWindowData.FrameIndex].CommandPool;
    VkCommandBuffer command_buffer;
    {
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandPool = command_pool;
        alloc_info.commandBufferCount = 1;

        err = vkAllocateCommandBuffers(g_Device, &alloc_info, &command_buffer);
        check_vk_result(err);

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(command_buffer, &begin_info);
        check_vk_result(err);
    }

    // Copy to Image
    {
        VkImageMemoryBarrier copy_barrier[1] = {};
        copy_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        copy_barrier[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copy_barrier[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        copy_barrier[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copy_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        copy_barrier[0].image = tex_data->Image;
        copy_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_barrier[0].subresourceRange.levelCount = 1;
        copy_barrier[0].subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, copy_barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = tex_data->width;
        region.imageExtent.height = tex_data->height;
        region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(command_buffer, tex_data->UploadBuffer, tex_data->Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier use_barrier[1] = {};
        use_barrier[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        use_barrier[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        use_barrier[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        use_barrier[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        use_barrier[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        use_barrier[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        use_barrier[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        use_barrier[0].image = tex_data->Image;
        use_barrier[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        use_barrier[0].subresourceRange.levelCount = 1;
        use_barrier[0].subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, use_barrier);
    }

    // End command buffer
    {
        VkSubmitInfo end_info = {};
        end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        end_info.commandBufferCount = 1;
        end_info.pCommandBuffers = &command_buffer;
        err = vkEndCommandBuffer(command_buffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &end_info, VK_NULL_HANDLE);
        check_vk_result(err);
        err = vkDeviceWaitIdle(g_Device);
        check_vk_result(err);
    }

    return true;
}

void RemoveTexture(TextureData* tex_data) {
    vkFreeMemory(g_Device, tex_data->UploadBufferMemory, nullptr);
    vkDestroyBuffer(g_Device, tex_data->UploadBuffer, nullptr);
    vkDestroySampler(g_Device, tex_data->Sampler, nullptr);
    vkDestroyImageView(g_Device, tex_data->ImageView, nullptr);
    vkDestroyImage(g_Device, tex_data->Image, nullptr);
    vkFreeMemory(g_Device, tex_data->ImageMemory, nullptr);
    ImGui_ImplVulkan_RemoveTexture(tex_data->DS);
}

static int ResizeCallback(ImGuiInputTextCallbackData* data) { // for string usage inside ImGui::InputText()
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        // user-data is the std::string pointer
        std::string* str = (std::string*)data->UserData;
        str->resize(data->BufTextLen); // resize string to new text length (not counting null)
        data->Buf = const_cast<char*>(str->c_str()); // update ImGui's buffer pointer
    }
    return 0;
}

int Interface::Render(std::atomic<bool>* runningFlag) {
    // configuration path
    char config_path[PATH_MAX];
    char image_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/%s", getenv("HOME"), ".config/konacode/konamask/config.ini");
    snprintf(image_path, sizeof(image_path), "%s/%s", getenv("HOME"), ".config/konacode/konamask/background");
    // create window with Vulkan graphics context
    float main_scale = ImGui_ImplSDL2_GetContentScaleForDisplay(0);
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    window = SDL_CreateWindow("konamask", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)(1280 * main_scale), (int)(720 * main_scale), window_flags);
    if (window == nullptr) {
        printf("[ERROR] (Vulkan/SDL2) SDL_CreateWindow(): %s\n", SDL_GetError());
        return -1;
    }
    std::cout << "[INFO] (Vulkan/SDL2) Window created successfully!" << std::endl;

    ImVector<const char*> extensions;
    uint32_t extensions_count = 0;
    SDL_Vulkan_GetInstanceExtensions(window, &extensions_count, nullptr);
    extensions.resize(extensions_count);
    SDL_Vulkan_GetInstanceExtensions(window, &extensions_count, extensions.Data);
    VkSetup(extensions);

    // create window surface
    VkSurfaceKHR surface;
    VkResult err;
    if (SDL_Vulkan_CreateSurface(window, g_Instance, &surface) == 0) {
        printf("[ERROR] (Vulkan/SDL2) Failed to create Vulkan/SDL2 surface.\n");
        return 1;
    }
    std::cout << "[INFO] (Vulkan/SDL2) Successfully created Vulkan/SDL2 surface!" << std::endl;

    // create framebuffers
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVkWindow(wd, surface, w, h);

    // setup ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // enable keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // enable gamepad Controls

    // setup ImGui style
    //ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // apply user configuration
    float tcr;
    float tcg;
    float tcb;
    // validate range & convert to normalized floats
    auto in_range = [](int v){ return (v >= 0 && v <= 255); };
    if (!in_range(cfg.get<int>("ui_theme_red",   50)) || !in_range(cfg.get<int>("ui_theme_green", 20)) || !in_range(cfg.get<int>("ui_theme_blue",  60))) {
        std::cerr << "[ERROR] Color parse/validation failed, reverting to defaults.\n";
        tcr = static_cast<float>(50); tcg = static_cast<float>(20); tcb = static_cast<float>(60);
    } 
    else {
        // convert to normalized floats 0.0 - 1.0
        tcr = static_cast<float>(cfg.get<int>("ui_theme_red",   167)) / 255.0f;
        tcg = static_cast<float>(cfg.get<int>("ui_theme_green", 42)) / 255.0f;
        tcb = static_cast<float>(cfg.get<int>("ui_theme_blue",  92)) / 255.0f;
    }
    std::cout << "[INFO] Setting ImGui style theme color as following:\n[INFO] Red: " << tcr*255 << "  | (" << tcr << ")\n[INFO] Green: " << tcg*255 << "  | (" << tcg << ")\n[INFO] Blue: " << tcb*255 << "  | (" << tcb << ")" << std::endl; 
    // setup scaling
    ImVec4 theme_color(tcr, tcg, tcb, 1.0f); // only needed for the GUI

    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_Text]                  = ImVec4(0.86f, 0.93f, 0.89f, 0.78f);
    style.Colors[ImGuiCol_TextDisabled]          = ImVec4(0.86f, 0.93f, 0.89f, 0.28f);
    style.Colors[ImGuiCol_WindowBg]              = ImVec4(0.05f, 0.06f, 0.12f, 0.86f);
    style.Colors[ImGuiCol_Border]                = ImVec4(0.02f, 0.03f, 0.09f, 0.86f);
    style.Colors[ImGuiCol_BorderShadow]          = ImVec4(0.02f, 0.03f, 0.09f, 0.00f);
    style.Colors[ImGuiCol_FrameBg]               = ImVec4(0.20f, 0.22f, 0.27f, 0.78f);
    style.Colors[ImGuiCol_FrameBgHovered]        = ImVec4(tcr, tcg, tcb, 0.45f);
    style.Colors[ImGuiCol_FrameBgActive]         = ImVec4(tcr, tcg, tcb, 0.72f);
    style.Colors[ImGuiCol_TitleBg]               = ImVec4(0.20f, 0.22f, 0.27f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.20f, 0.22f, 0.27f, 0.75f);
    style.Colors[ImGuiCol_TitleBgActive]         = ImVec4(tcr, tcg, tcb, 0.86f);
    style.Colors[ImGuiCol_MenuBarBg]             = ImVec4(0.20f, 0.22f, 0.27f, 0.47f);
    style.Colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.20f, 0.22f, 0.27f, 0.00f);
    style.Colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.09f, 0.15f, 0.16f, 0.86f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(tcr, tcg, tcb, 0.36f);
    style.Colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(tcr, tcg, tcb, 0.86f);
    style.Colors[ImGuiCol_CheckMark]             = ImVec4(tcr, tcg, tcb, 1.00f);
    style.Colors[ImGuiCol_SliderGrab]            = ImVec4(tcr, tcg, tcb, 0.96f);
    style.Colors[ImGuiCol_SliderGrabActive]      = ImVec4(tcr, tcg, tcb, 0.10f);
    style.Colors[ImGuiCol_Button]                = ImVec4(0.47f, 0.77f, 0.83f, 0.14f);
    style.Colors[ImGuiCol_ButtonHovered]         = ImVec4(tcr, tcg, tcb, 0.86f);
    style.Colors[ImGuiCol_ButtonActive]          = ImVec4(tcr, tcg, tcb, 1.00f);
    style.Colors[ImGuiCol_Header]                = ImVec4(tcr, tcg, tcb, 0.76f);
    style.Colors[ImGuiCol_HeaderHovered]         = ImVec4(tcr, tcg, tcb, 0.86f);
    style.Colors[ImGuiCol_HeaderActive]          = ImVec4(tcr, tcg, tcb, 0.86f);
    style.Colors[ImGuiCol_Separator]             = ImVec4(0.14f, 0.16f, 0.19f, 1.00f);
    style.Colors[ImGuiCol_SeparatorHovered]      = ImVec4(tcr, tcg, tcb, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive]       = ImVec4(tcr, tcg, tcb, 1.00f);
    style.Colors[ImGuiCol_ResizeGrip]            = ImVec4(0.47f, 0.77f, 0.83f, 0.04f);
    style.Colors[ImGuiCol_ResizeGripHovered]     = ImVec4(tcr, tcg, tcb, 0.78f);
    style.Colors[ImGuiCol_ResizeGripActive]      = ImVec4(tcr, tcg, tcb, 1.00f);
    style.Colors[ImGuiCol_PlotLines]             = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    style.Colors[ImGuiCol_PlotLinesHovered]      = ImVec4(tcr, tcg, tcb, 1.00f);
    style.Colors[ImGuiCol_PlotHistogram]         = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    style.Colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(tcr, tcg, tcb, 1.00f);
    style.Colors[ImGuiCol_TextSelectedBg]        = ImVec4(tcr, tcg, tcb, 0.45f);
    style.Colors[ImGuiCol_PopupBg]               = ImVec4(0.20f, 0.22f, 0.27f, 0.9f);
    style.ScaleAllSizes(main_scale);    // bake a fixed style scale
    io.FontGlobalScale = main_scale;               // scales all fonts by main_scale
    io.Fonts->Clear();
    if (cfg.get<std::string>("ui_custom_font", "") != "disable" || cfg.get<std::string>("ui_custom_font", "") != "false") {
        io.Fonts->AddFontFromFileTTF(cfg.get<std::string>("ui_custom_font", "").c_str(), cfg.get<int>("ui_font_size", 24) * main_scale);
    }
    else {
        std::cout << "[INFO] Font parameter was disabled, using default!" << std::endl;
        io.Fonts->AddFontDefault();
    }
    
    
    // setup platform/renderer backends
    ImGui_ImplSDL2_InitForVulkan(window);
    
    ImGui_ImplVulkan_InitInfo init_info = {};

    //init_info.ApiVersion = VK_API_VERSION_1_3;              // pass in value of VkApplicationInfo::apiVersion, otherwise will default to header version.
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.RenderPass = wd->RenderPass;
    init_info.Subpass = 0;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = g_Allocator;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    // ─────────────── manual font‐upload ───────────────
    {
        // manual font upload
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels; int w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);

        // create staging buffer
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        createBuffer(g_Device, g_PhysicalDevice,
                     w * h * 4,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer, stagingBufferMemory);
        
        // copy pixel data
        void* data;
        vkMapMemory(g_Device, stagingBufferMemory, 0, VK_WHOLE_SIZE, 0, &data);
        memcpy(data, pixels, (size_t)(w * h * 4));
        vkUnmapMemory(g_Device, stagingBufferMemory);
        
        // create the GPU image
        VkDeviceMemory fontImageMemory;
        VkImage fontImage = CreateFontImage(g_Device, g_PhysicalDevice, w, h, fontImageMemory);
        
        // transition, copy & transition back
        VkCommandBuffer cmd = beginSingleTimeCommands(g_Device, g_CommandPool);
        transitionImageLayout(cmd, fontImage, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy region{ };
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0,0,1 };
        region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
        vkCmdCopyBufferToImage(cmd, stagingBuffer, fontImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        transitionImageLayout(cmd, fontImage, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        endSingleTimeCommands(g_Device, g_CommandPool, g_Queue, cmd);
        
        // cleanup staging
        vkDestroyBuffer(g_Device, stagingBuffer, nullptr);
        vkFreeMemory(g_Device, stagingBufferMemory, nullptr);
        
        // create view & sampler
        VkImageView fontView = CreateFontImageView(g_Device, fontImage);
        VkSampler   fontSampler = CreateFontSampler(g_Device);
        
        // register with ImGui
        VkDescriptorSet desc = ImGui_ImplVulkan_AddTexture(
            fontSampler, fontView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
        
        io.Fonts->SetTexID((ImTextureID)desc);
        io.Fonts->ClearTexData();

    }
    // ───────────────────────────────────────────────────

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
    // configuration variables (currently no way of avoiding)
    //tts
    unsigned short sr = cfg.get<int>("speech_rate", 150);
    unsigned short sp = cfg.get<int>("speech_pitch", 50);
    unsigned short sv = cfg.get<int>("speech_volume", 100);
    unsigned int sthreshold = cfg.get<int>("silence_threshold", 200);
    unsigned int stimeout = cfg.get<int>("silence_timeout", 1000);
    //stt
    std::string voskapi = cfg.get<std::string>("voskapi_model_path", "./model");
    voskapi.reserve(256);
    voskapi.resize(voskapi.size());
    std::string voicebank = cfg.get<std::string>("speech_vociebank", "en-us");
    voicebank.reserve(256);
    voicebank.resize(voicebank.size());
    // advanced
    int pasamplerate = cfg.get<int>("pa_sample_spec_rate", 22050);
    double bufferfactor = cfg.get<double>("buffer_factor", 0.05);

    // rendering variables
    bool settings = false;
    bool budgetfm = false;    
    char bgsuccess = 'u'; // unset
    bool manual = false;
    bool debug_log = false;
    bool stats = cfg.get<bool>("enable_statistics", false);
    //bool imgbg = (cfg.get<bool>("enable_custom_background", false) &&CheckFile(image_path)); throws (idk why)
    bool imgbg = (CheckFile(image_path));
    float r;
    float g;
    float b;
    
    char usr[PATH_MAX];
    snprintf(usr, sizeof(usr), "%s", getenv("HOME"));
    static char manInput[128] = "";
    // validate range & convert to normalized floats
    //auto in_range = [](int v){ return (v >= 0 && v <= 255); }; defined above
    if (!in_range(cfg.get<int>("ui_bgc_red",   50)) || !in_range(cfg.get<int>("ui_bgc_green", 20)) || !in_range(cfg.get<int>("ui_bgc_blue",  60))) {
        std::cerr << "[ERROR] Color parse/validation failed, reverting to defaults.\n";
        r = static_cast<float>(50); g = static_cast<float>(20); b = static_cast<float>(60);
    } 
    else {
        // convert to normalized floats 0.0 - 1.0
        r = static_cast<float>(cfg.get<int>("ui_bgc_red",   50)) / 255.0f;
        g = static_cast<float>(cfg.get<int>("ui_bgc_green", 20)) / 255.0f;
        b = static_cast<float>(cfg.get<int>("ui_bgc_blue",  60)) / 255.0f;
    }
    std::cout << "[INFO] Setting SDL2 background color as following:\n[INFO] Red: " << r*255 << "\n[INFO] Green: " << g*255 << "\n[INFO] Blue: " << b*255 << std::endl; 
    ImVec4 backgorund_color(r, g, b, 1.0f);
    // load user background image
    TextureData texture;
    bool ret = LoadTextureFromFile(image_path, &texture);
    if (imgbg) {
        IM_ASSERT(ret);
    }

    ImGuiFilePicker picker;
    std::string out_path;
    SDL_Event event;
    int fb_width, fb_height;
    while (runningFlag->load()) {
        
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                break;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                break;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }
        if (g_RenderPaused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        // resize swap chain?
        SDL_GetWindowSize(window, &fb_width, &fb_height);
        if (fb_width > 0 && fb_height > 0 && (g_SwapChainRebuild || g_MainWindowData.Width != fb_width || g_MainWindowData.Height != fb_height)) {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, &g_MainWindowData, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }

        // start the ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();


            if (settings) {
                ImGui::Begin("Settings", &settings);
                ImGui::SeparatorText("dashboard customization");

                if (imgbg) { 
                    ImGui::Text("Change background image:");
                    if (budgetfm) {
                        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
                        ImGui::Button("Select new background");
                        ImGui::PopItemFlag();
                        ImGui::PopStyleVar();
                        if (picker.Draw("pick an image", &budgetfm, &out_path)) {
                            printf("[INFO] Selected new background: %s\n[INFO] Replacing the current image...\n", out_path.c_str());
                                if (CopyFile(out_path,image_path)) {
                                    RemoveTexture(&texture);
                                    ret = LoadTextureFromFile(image_path, &texture);
                                    IM_ASSERT(ret);
                                    std::cout << "[INFO] Applied new image successfully!" << std::endl;
                                    bgsuccess = 's'; // set
                                } else { std::cout << "[ERROR] Could not update the background image!" << std::endl; bgsuccess = 'f'; } // fail
                            budgetfm = false;
                        }
                    }
                    else {
                        if (ImGui::Button("Select new background"))
                            budgetfm = true;
                        if (bgsuccess == 's') {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(18,192,18,255));
                            ImGui::Text("Successfully updated the background!");
                            ImGui::Spacing();
                            ImGui::PopStyleColor();
                        } else if (bgsuccess == 'f') {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(242,37,85,255));
                            ImGui::Text("Failed to update the background!");
                            ImGui::Spacing();
                            ImGui::PopStyleColor();                        
                        }
                    }


                } 
                else {
                    ImGui::Text("Change background color:");
                    ImGui::ColorEdit3("", (float*)&backgorund_color);
                }
                ImGui::Text("Change theme color:");
                ImGui::ColorEdit3("", (float*)&theme_color);
                //ImGui::InputTextWithHint("Vosk-API Model", "Vosk-API Model Path", cfg.<std::string>("voskapi_model_path").c_str(), IM_ARRAYSIZE(cfg.get<std::string>("voskapi_model_path").c_str()));
                ImGui::SeparatorText("text-to-speech");
                ImGui::InputInt("Speech rate", (int*)&sr);
                ImGui::InputInt("Speech pitch", (int*)&sp);
                ImGui::InputInt("Speech volume", (int*)&sv); ImGui::Spacing();
                ImGui::InputInt("Silence threshold", (int*)&sthreshold);
                ImGui::InputInt("Silence timeout", (int*)&stimeout);
                ImGui::SeparatorText("speech-to-text");
                ImGui::Text("Vosk-API model");
                ImGui::InputText("Folder path", const_cast<char*>(voskapi.c_str()), voskapi.capacity()+1, flags, ResizeCallback, (void*)&voskapi);
                ImGui::Text("Voicebank");
                ImGui::InputText("Voicebank name", const_cast<char*>(voicebank.c_str()), voicebank.capacity()+1, flags, ResizeCallback, (void*)&voicebank);
                ImGui::SeparatorText("advanced parameters");
                ImGui::Text("Do not change, unless you know, what you're doing."); ImGui::Spacing();
                ImGui::InputInt("PulseAudio sample rate", (int*)&pasamplerate);
                ImGui::InputDouble("PortAudio buffer factor", (double*)&bufferfactor);
                ImGui::SeparatorText("");
                if (ImGui::Button("Save")) {
                    std::cout << "[INFO] Saving settings..." << std::endl;
                    ImVec4ToFloats({r,g,b,0});
                    if (!imgbg) {
                        try {
                            cfg.set<int>("ui_bgc_red", r*255);
                            cfg.set<int>("ui_bgc_green", g*255);
                            cfg.set<int>("ui_bgc_blue", b*255);
                            std::cout << "[INFO] Background color updated successfully!" << std::endl;
                        }
                        catch (...) {   std::cout << "[ERROR] Unable to update background color configuration! Skipping..." << std::endl; }
                        
                    } else { std::cout << "[INFO] Skipping background color saving due to background image being active." << std::endl; }
                    ImVec4ToFloats({tcr,tcg,tcb,0});
                    try {
                        cfg.set<int>("ui_theme_red", tcr*255);
                        cfg.set<int>("ui_theme_green", tcg*255);
                        cfg.set<int>("ui_theme_blue", tcb*255);
                        std::cout << "[INFO] Theme color updated successfully!" << std::endl;
                        
                    }
                    catch (...) {   std::cout << "[ERROR] Unable to update theme color configuration! Skipping..." << std::endl; }
                    if (cfg.SaveToFile(config_path)) {
                        std::cout << "[INFO] Successfully applied all settings!" << std::endl;
                    } else { std::cout << "[ERROR] Unable to save settings: an unexpected exception occured! - I the file in use of another proces?" << std::endl; }
                } ImGui::SameLine();
                if (ImGui::Button("Close"))
                    settings = false;
                ImGui::End();
        }
        if (manual) {
            ImGui::Begin("Manual voice output", &manual);
            ImGui::Text("Manual voice output:");
            ImGui::InputText("Limit: 128 characters", (char*)&manInput, IM_ARRAYSIZE(manInput));
            if (ImGui::Button("Speak through the microphone")) {
                TextToSpeech::Verbalize((char*)&manInput);
            }
            ImGui::Spacing();
            if (ImGui::Button("Close"))
                manual = false;
            ImGui::End();
        }
        if (debug_log) {
            ImGui::ShowDebugLogWindow();
        }
        if (imgbg) {
            // remove padding
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            {
                ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2((float)fb_width,(float)fb_height), ImGuiCond_Always);
            
                ImGui::Begin("background", nullptr,
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoInputs |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoBackground);
                
                
                ImGui::Image(
                    (ImTextureID)texture.DS, 
                    ImVec2(fb_width, fb_height), 
                
                    ImVec2(ImClamp(((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f) - fb_width) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f))), 0.0f, 1.0f), ImClamp(((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f) - fb_height) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f))), 0.0f, 1.0f)), 
                    ImVec2(ImClamp(1.0f - ((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f) - fb_width) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.width > 0) ? (float)texture.width : 1.0f))), 0.0f, 1.0f), ImClamp(1.0f - ((std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f) - fb_height) / (2.0f * std::max(fb_width/((texture.width > 0) ? (float)texture.width : 1.0f), fb_height/((texture.height > 0) ? (float)texture.height : 1.0f))*((texture.height > 0) ? (float)texture.height : 1.0f))), 0.0f, 1.0f)));
                
                
                
                ImGui::End();
            }
            ImGui::PopStyleVar();
        }
        {
            
            ImGui::SetNextWindowPos(ImVec2(0,0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2((float)fb_width,(float)fb_height), ImGuiCond_Always);
            static float f = 0.0f;
            ImGui::Begin("konamask dashboard", 0, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
            ImGui::SetNextWindowSize(ImGui::GetWindowSize());
            ImGui::Text("konamask voice transforming utility");
            ImGui::Checkbox("open settings", &settings);
            ImGui::Checkbox("open manual voice output", &manual);
            ImGui::Checkbox("open ImGui debug log", &debug_log);

            ImGui::SliderFloat("float", &f, 0.0f, 1.0f);

            if (stats) { ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate); }
            if (ImGui::Button("Minimize")) {
                Minimize();
            }            
            if (ImGui::Button("Exit")) {
                if (!Shutdown(surface)) {
                    std::cout << "[ERROR] (Vulkan/SDL2) Unable to shutdown properly!" << std::endl;
                    std::exit(EXIT_FAILURE);
                }
            }
            ImGui::End();

        }


        // rendering
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f);
        if (!is_minimized)
        {
            wd->ClearValue.color.float32[0] = backgorund_color.x;
            wd->ClearValue.color.float32[1] = backgorund_color.y;
            wd->ClearValue.color.float32[2] = backgorund_color.z;
            FrameRender(wd, draw_data);
            FramePresent(wd);
        }
    }

    // cleanup
    if (!Shutdown(surface)) {
        std::cout << "[ERROR] (Vulkan/SDL2) Unable to shutdown properly!" << std::endl;
    }

    return 0;
}

int Interface::Initialize() {
    std::cout << "\n>────────────────[INITIALIZING GRAPHICAL USER INTERFACE]────────────────<\n" << std::endl;
    // setup SDL
#ifdef _WIN32
    ::SetProcessDPIAware();
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        printf("[ERROR] (Vulkan/SDL2) %s\n", SDL_GetError());
        return -1;
    }
    std::cout << "[INFO] (Vulkan/SDL2) SDL dependencies loaded successfully!" << std::endl;

// from 2.0.18: Enable native IME
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif


    std::cout << "\n>───────────[INITIALIZED GRAPHICAL USER INTERFACE SUCCESSULLY]──────────<\n" << std::endl;
    return 0;
}

