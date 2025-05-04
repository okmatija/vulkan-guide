#include <vk_pipelines.h>
#include <vk_initializers.h>
#include <fstream>


bool vkutil::load_shader_module(const char* path, VkDevice device, VkShaderModule* shader_module) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    size_t file_size = (size_t)file.tellg();

    // Reserve a buffer to store the compiled shader data, spir-v expects the buffer to be u32
    std::vector<uint32_t> buffer(file_size / sizeof(uint32_t));

    file.seekg(0);
    file.read((char*)buffer.data(), file_size);
    file.close();

    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.pNext = nullptr;
    info.codeSize = buffer.size() * sizeof(uint32_t);  // codeSize must be in bytes
    info.pCode = buffer.data();

    VkShaderModule created_module;
    if (vkCreateShaderModule(device, &info, nullptr, &created_module) != VK_SUCCESS) {
        return false;
    }
    *shader_module = created_module;
}
