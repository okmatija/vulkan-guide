#pragma once

#include <vk_types.h>

struct DescriptorLayoutBuilder {
    std::vector<VkDescriptorSetLayoutBinding> bindings;

    void add_binding(uint32_t binding, VkDescriptorType type);
    void clear();
    VkDescriptorSetLayout build(
        VkDevice device,
        VkShaderStageFlags shader_stages,
        void* p_next = nullptr,
        VkDescriptorSetLayoutCreateFlags flags = 0);
};

// Descriptor allocation happens via VkDescriptorPool, which needs to be initialized with specific
// sizes/types of descriptor. You could have one of these for the whole engine but they you'd need
// to know in advance what you will be using.
struct DescriptorAllocator {
    struct PoolSizeRatio {
        VkDescriptorType type;
        float ratio;
    };

    VkDescriptorPool pool;

    void init_pool(VkDevice device, uint32_t max_sets, std::span<PoolSizeRatio> pool_ratios);
    void clear_descriptors(VkDevice device);
    void destroy_pool(VkDevice device);

    VkDescriptorSet allocate(VkDevice device, VkDescriptorSetLayout layout);
};
