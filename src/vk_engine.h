
// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.
//> intro
#pragma once

#include <deque>
#include <functional>

#include <vk_descriptors.h>
#include <vk_initializers.h>
#include <vk_types.h>

#include "vk_mem_alloc.h"
#include "VkBootstrap.h"

// Better implementation is to store Vulkan handles (VkImage, VkBuffer, ...) and delete using those
struct DeletionQueue {
	std::deque<std::function<void()>> deletors;

	// void push_function(std::function<void()>&& function) {
	// 	deletors.emplace_back(std::move(function)); // FIXME(okmatija) this was push_back without std::move in the original
	// }

	void push_function(std::function<void()>&& function) {
		deletors.push_back(function);
	}

	void flush() {
		for (auto it = deletors.rbegin(); it != deletors.rend(); it++) {
			(*it)();
		}
		deletors.clear();
	}
};

struct FrameData {
	VkCommandPool _command_pool; // Must not be used concurrently in multiple threads
	VkCommandBuffer _main_command_buffer; // Note: Just an opaque (64bit) handle
	VkSemaphore _swapchain_semaphore; // So render commands wait on swapchain image request
	VkSemaphore _render_semaphore; // So image can be presented to the OS once drawing finishes
	VkFence _render_fence; // So we can wait for drawing commands in a given frame to finish

	DeletionQueue _deletion_queue;
};

constexpr unsigned int FRAME_OVERLAP = 2;

class VulkanEngine {
public:

	bool _isInitialized{ false };
	int _frameNumber{ 0 };
	bool stop_rendering{ false };
	VkExtent2D _windowExtent{ 1700 , 900 };

	struct SDL_Window* _window{ nullptr };

	static VulkanEngine& Get();

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	//draw loop
	void draw();

	//run main loop
	void run();

	VkInstance _instance; // Vulkan library handle
	VkDebugUtilsMessengerEXT _debug_messenger; // Vulkan debug output handle
	VkPhysicalDevice _chosenGPU; // GPU chosen as the default device
	VkDevice _device; // Vulkan device for commands
	VkSurfaceKHR _surface; // Vulkan window surface

	VkSwapchainKHR _swapchain;
	VkFormat _swapchain_image_format;
	std::vector<VkImage> _swapchain_images; // VkImage is the actual images in the swapchain
	std::vector<VkImageView> _swapchain_image_views; // VkImageView is a wrapper to do things like swap colors etc...
	VkExtent2D _swapchain_extent;

	FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP]; }

	DescriptorAllocator global_descriptor_allocator;
	VkDescriptorSet _draw_image_descriptors;
	VkDescriptorSetLayout _draw_image_descriptor_layout;

private:

	FrameData _frames[FRAME_OVERLAP];
	VkQueue _graphics_queue;
	uint32_t _graphics_queue_family;

	DeletionQueue _main_deletion_queue;
	VmaAllocator _allocator;

	// Draw resources
	// We do not draw directly into the swapchain since across platforms the formats are not
	// guaranteed and are limited to the resolution of the window so if you want to rendering
	// higher or lower resolution and then scale you need a different image. Swapchain formats
	// are also usually low precision (8 bits per color) which prevents you doing high
	// precision lighting etc.
	AllocatedImage _draw_image;
	VkExtent2D _draw_extent;

	void init_vulkan();
	void init_swapchain();
	void init_commands();
	void init_sync_structures();
	void init_descriptors();

	void draw_background(VkCommandBuffer cmd);

	void create_swapchain(uint32_t width, uint32_t height);
	void destroy_swapchain();
};
//< intro