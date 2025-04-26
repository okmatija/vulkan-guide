//> includes
#include "vk_engine.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vk_initializers.h>
#include <vk_images.h>
#include <vk_types.h>

#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

#include <chrono>
#include <thread>
//< includes

//> init
constexpr bool bUseValidationLayers = true;

VulkanEngine* loadedEngine = nullptr;

VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }
void VulkanEngine::init()
{
    // only one engine initialization is allowed with the application.
    assert(loadedEngine == nullptr);
    loadedEngine = this;

    // We initialize SDL and create a window with it.
    SDL_Init(SDL_INIT_VIDEO);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN);

    _window = SDL_CreateWindow(
        "Vulkan Engine",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        _windowExtent.width,
        _windowExtent.height,
        window_flags);

    init_vulkan();
    init_swapchain();
    init_commands();
    init_sync_structures();

    // everything went fine
    _isInitialized = true;
}
//< init

//> extras
void VulkanEngine::cleanup()
{
    if (_isInitialized) {
        // Make sure the GPU has stopped doing stuff
        vkDeviceWaitIdle(_device);

        // Free per-frame data and the deletion queue
        for (int i = 0; i < FRAME_OVERLAP; i++) {
            vkDestroyCommandPool(_device, _frames[i]._command_pool, nullptr);

            // Destroy sync objects
            vkDestroyFence(_device, _frames[i]._render_fence, nullptr);
            vkDestroySemaphore(_device, _frames[i]._render_semaphore, nullptr);
            vkDestroySemaphore(_device, _frames[i]._swapchain_semaphore, nullptr);

            _frames[i]._deletion_queue.flush();
        }

        _main_deletion_queue.flush();

        // Destroy in the reverse order of creation
        destroy_swapchain();
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        vkDestroyDevice(_device, nullptr);
        vkb::destroy_debug_utils_messenger(_instance, _debug_messenger);
        vkDestroyInstance(_instance, nullptr);
        SDL_DestroyWindow(_window);
    }

    // clear engine pointer
    loadedEngine = nullptr;
}

void VulkanEngine::draw_background(VkCommandBuffer cmd) {
    VkClearColorValue clear_value;
    float flash = std::abs(std::sin(_frameNumber / 120.f));
    clear_value = { {0.f, 0.f, flash, 1.f} };

    VkImageSubresourceRange clear_range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

    vkCmdClearColorImage(cmd, _draw_image.image, VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1, &clear_range);
}

void VulkanEngine::draw()
{
    // Wait for GPU to render the last frame with a timeout of 1 second
    // Tip: Call vkWaitForFences with timeout_ns=0 to find out if the GPU is still executing the command
    uint64_t timeout_ns = 1'000'000'000;
    VkBool32 wait_all = true;
    VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame()._render_fence, wait_all, timeout_ns));

    // GPU has finished executing the frame after the fence so we can delete objects created for the frame
    get_current_frame()._deletion_queue.flush();

    VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._render_fence));

    // Request image from the swapchain
    uint32_t swapchain_image_index;
    VK_CHECK(vkAcquireNextImageKHR(_device, _swapchain, timeout_ns,
        get_current_frame()._swapchain_semaphore, // Passing this ensures we can sync with other operations and have an image ready to render
        nullptr,
        &swapchain_image_index));

    VkCommandBuffer cmd = get_current_frame()._main_command_buffer; // alias TODO(okmatija) rename to buffer

    // Reset the command buffer to record again (we know from above that the commands finished executing)
    VK_CHECK(vkResetCommandBuffer(cmd, 0));

    VkCommandBufferBeginInfo cmd_begin_info = vkinit::command_buffer_begin_info(
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT); // Flag to tell the driver we'll be submitting and executing once, maybe we'll get a speedup.

    //
    // Command buffer recording
    //
    {
        _draw_extent.width = _draw_image.imageExtent.width;
        _draw_extent.height = _draw_image.imageExtent.height;

        VK_CHECK(vkBeginCommandBuffer(cmd, &cmd_begin_info));

        // Make the draw image writable before rendering, this put in the command buffer
        // More details on image layouts: https://docs.vulkan.org/spec/latest/chapters/resources.html#resources-image-layouts
        vkutil::transition_image(cmd, _draw_image.image,
            VK_IMAGE_LAYOUT_UNDEFINED, // Note: a "dont care" state and the state of new images. We dont care about what the older layout was
            VK_IMAGE_LAYOUT_GENERAL);

        draw_background(cmd);

        // Transition the draw image and the swapchain image into their correct transfer layouts
        vkutil::transition_image(cmd, _draw_image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkutil::transition_image(cmd, _swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        // Execute a copy from the draw image to the swapchain
        // Note: this function uses copying (fast but inflexible, need matching sizes and pixel formats) and
        // blitting (more flexible but slower, can scale, filter or change formats). We could also use a
        // fragment shader for the most flexiblity.
        vkutil::copy_image_to_image(cmd, _draw_image.image, _swapchain_images[swapchain_image_index], _draw_extent, _swapchain_extent);

        // Set swapchain image layout to Present so we can show it on the screen
        vkutil::transition_image(cmd, _swapchain_images[swapchain_image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        // Finalize the command buffer (we can no longer add commands but it can be executed)
        VK_CHECK(vkEndCommandBuffer(cmd));
    }

    //
    // Prepare submission to the queue.
    // We will wait on _swapchain_semaphore (which signals when the swapchain is ready)
    // We will signal _render_semaphore to signal rendering has finished
    //

    {
        VkCommandBufferSubmitInfo cmd_info = vkinit::command_buffer_submit_info(cmd);
        VkSemaphoreSubmitInfo wait_info = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, get_current_frame()._swapchain_semaphore);
        VkSemaphoreSubmitInfo signal_info = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, get_current_frame()._render_semaphore);
        VkSubmitInfo2 submit = vkinit::submit_info(&cmd_info, &signal_info, &wait_info);

        // Submit command buffer to the queue to execute it
        // _render_fence will block until the command finishes execution
        VK_CHECK(vkQueueSubmit2(_graphics_queue, 1, &submit, get_current_frame()._render_fence));
    }

    //
    // Prepare present
    //

    {
        VkPresentInfoKHR present_info = {};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.pNext = nullptr;
        present_info.pSwapchains = &_swapchain;
        present_info.swapchainCount = 1;
        present_info.pWaitSemaphores = &get_current_frame()._render_semaphore; // Don't present the image until the rendering commands are done from the submit above
        present_info.waitSemaphoreCount = 1;

        present_info.pImageIndices = &swapchain_image_index;

        VK_CHECK(vkQueuePresentKHR(_graphics_queue, &present_info));
    }

    _frameNumber++;
}

//< extras

//> drawloop
void VulkanEngine::run()
{
    SDL_Event e;
    bool bQuit = false;

    // main loop
    while (!bQuit) {
        // Handle events on queue
        while (SDL_PollEvent(&e) != 0) {
            // close the window when user alt-f4s or clicks the X button
            if (e.type == SDL_QUIT)
                bQuit = true;

            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    stop_rendering = true;
                }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
                    stop_rendering = false;
                }
            }
        }

        // do not draw if we are minimized
        if (stop_rendering) {
            // throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        draw();
    }
}
//< drawloop

void VulkanEngine::init_vulkan() {
    // This abstracts the creation of VkInstance
    vkb::InstanceBuilder builder;
    vkb::Result<vkb::Instance> inst_ret = builder.set_app_name("Start from Chapter 0") // The app_name is mostly for driver developers, so they can tweak internal driver code on the app.
        .request_validation_layers(bUseValidationLayers)
        .use_default_debug_messenger()
        .require_api_version(1, 3, 0)
        .build();

    vkb::Instance vkb_instance = inst_ret.value();

    // Set some handles
    _instance = vkb_instance.instance;
    _debug_messenger = vkb_instance.debug_messenger;
    SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

    //
    // Select features
    //

    VkPhysicalDeviceVulkan13Features features{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    features.dynamicRendering = true; // For skipping renderpasses and framebuffers
    features.synchronization2 = true; // For new syncronization functions

    VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    features12.bufferDeviceAddress = true; // So we can use GPU pointers without binding buffers
    features12.descriptorIndexing = true; // Gives us bindless textures

    //
    // Init device handles
    //

    // Choose a gpu which can write to the SDL surface and supports vulkan 1.3 with the chosen features
    vkb::PhysicalDeviceSelector selector{ vkb_instance };
    vkb::PhysicalDevice physicalDevice = selector
        .set_minimum_version(1, 3)
        .set_required_features_13(features)
        .set_required_features_12(features12)
        .set_surface(_surface)  // We need to chose a device that can render to the window we want
        .select()
        .value();

    vkb::DeviceBuilder deviceBuilder{ physicalDevice };
    vkb::Device vkbDevice = deviceBuilder.build().value();

    _device = vkbDevice.device;  // Once we have a physical device we can get a VkDevice from it 
    _chosenGPU = physicalDevice.physical_device;

    _graphics_queue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    _graphics_queue_family = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

    // Initialize the memory allocator

    VmaAllocatorCreateInfo allocator_info = {};
    allocator_info.physicalDevice = _chosenGPU;
    allocator_info.device = _device;
    allocator_info.instance = _instance;
    allocator_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&allocator_info, &_allocator);

    // Destroy the allocator when the engine exits
    _main_deletion_queue.push_function([&]() {
        vmaDestroyAllocator(_allocator);
        });
}

void VulkanEngine::init_swapchain() {
    create_swapchain(_windowExtent.width, _windowExtent.height);

    // Draw image size matches the window
    VkExtent3D draw_image_extent = {
        _windowExtent.width,
        _windowExtent.height,
        1
    };

    // Hardcoding the draw format, 16 bit floats for each channel. This is slightly overkill but gives us lots of
    // extra precision for lighting and better rendering.
    _draw_image.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    _draw_image.imageExtent = draw_image_extent;

    // In Vulkan all images/buffers must set usage flags so the driver can perform optimizations
    VkImageUsageFlags draw_image_usages{};
    // So we can copy from and into the image
    draw_image_usages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    draw_image_usages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // So we can write to the image from the compute shader
    draw_image_usages |= VK_IMAGE_USAGE_STORAGE_BIT;
    // So graphics pipelines can draw geometry into it
    draw_image_usages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    // Render image info
    VkImageCreateInfo rimg_info = vkinit::image_create_info(_draw_image.imageFormat, draw_image_usages, draw_image_extent);

    // For the draw image, we want to allocate it from GPU local memory
    VmaAllocationCreateInfo rimg_allocinfo = {};
    rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY; // We wont ever access the image on the CPU, so it can go to GPU VRAM
    rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); // Only gpu-side VRAM has this flag, so it guaratees the fastest access

    // Allocate and create the image
    vmaCreateImage(_allocator, &rimg_info, &rimg_allocinfo, &_draw_image.image, &_draw_image.allocation, nullptr);

    // Build an image-view for the draw image to use for rendering
    VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(_draw_image.imageFormat, _draw_image.image, VK_IMAGE_ASPECT_COLOR_BIT);

    VK_CHECK(vkCreateImageView(_device, &rview_info, nullptr, &_draw_image.imageView));

    // Add to the deletion queues
    _main_deletion_queue.push_function([=]() {
        vkDestroyImageView(_device, _draw_image.imageView, nullptr);
        vmaDestroyImage(_allocator, _draw_image.image, _draw_image.allocation);
        });
}

void VulkanEngine::init_commands() {
    // Create a command pool for commands submitted to the graphics queue.
    // We want the pool to allow resetting individual command buffers.
    VkCommandPoolCreateInfo command_pool_info = vkinit::command_pool_create_info(_graphics_queue_family, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    for (int i = 0; i < FRAME_OVERLAP; i++) {
        VK_CHECK(vkCreateCommandPool(_device, &command_pool_info, nullptr, &_frames[i]._command_pool));

        // Since we do single threaded command recording we only use the primary level (hardcoded in the following function)
        VkCommandBufferAllocateInfo alloc_info = vkinit::command_buffer_allocate_info(_frames[i]._command_pool, 1);

        VK_CHECK(vkAllocateCommandBuffers(_device, &alloc_info, &_frames[i]._main_command_buffer));
    }
}

void VulkanEngine::init_sync_structures() {
    // Fence starts signalled so we can wait on it on the first frame.
    VkFenceCreateInfo fence_create_info = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
    VkSemaphoreCreateInfo semaphore_create_info = vkinit::semaphore_create_info();
    for (int i = 0; i < FRAME_OVERLAP; i++) {
        VK_CHECK(vkCreateFence(_device, &fence_create_info, nullptr, &_frames[i]._render_fence));
        VK_CHECK(vkCreateSemaphore(_device, &semaphore_create_info, nullptr, &_frames[i]._render_semaphore));
        VK_CHECK(vkCreateSemaphore(_device, &semaphore_create_info, nullptr, &_frames[i]._swapchain_semaphore));
    }
}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height) {
    vkb::SwapchainBuilder swapchain_builder{ _chosenGPU, _device, _surface };
    _swapchain_image_format = VK_FORMAT_B8G8R8A8_UNORM;

    // We'll need to rebuild the swapchain if the window resizes
    vkb::Swapchain vkb_swapchain = swapchain_builder
        // .use_default_format_selection()
        .set_desired_format(VkSurfaceFormatKHR{ .format = _swapchain_image_format, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)  // Hard VSync: limit fps to the speed of the monitor
        .set_desired_extent(width, height)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .build().value();

    _swapchain = vkb_swapchain.swapchain;
    _swapchain_extent = vkb_swapchain.extent;
    _swapchain_images = vkb_swapchain.get_images().value();
    _swapchain_image_views = vkb_swapchain.get_image_views().value();
}

void VulkanEngine::destroy_swapchain() {
    // Destroy the swapchain object first which deletes the images it holds internally and then destroy the image views for those images
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    for (int i = 0; i < _swapchain_image_views.size(); i++) {
        vkDestroyImageView(_device, _swapchain_image_views[i], nullptr);
    }
}
