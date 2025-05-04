#pragma once 
#include <vk_types.h>

namespace vkutil {

    bool load_shader_module(const char* path, VkDevice device, VkShaderModule* shader_module);

}