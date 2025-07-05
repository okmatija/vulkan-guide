#pragma once 
#include <vk_types.h>

namespace vkutil {

    bool load_shader_module(const char* path, VkDevice device, VkShaderModule* shader_module);

    class PipelineBuilder {
    public:
        std::vector<VkPipelineShaderStageCreateInfo> _shader_stages;

        VkPipelineInputAssemblyStateCreateInfo _input_assembly;
        VkPipelineRasterizationStateCreateInfo _rasterizer;
        VkPipelineColorBlendAttachmentState _color_blend_attachment;
        VkPipelineMultisampleStateCreateInfo _multisampling;
        VkPipelineLayout _pipeline_layout;
        VkPipelineDepthStencilStateCreateInfo _depth_stencil;
        VkPipelineRenderingCreateInfo _render_info;
        VkFormat _color_attachment_format;

        PipelineBuilder() { clear(); }

        void set_shaders(VkShaderModule vertex_shader, VkShaderModule fragment_shader);
        void set_input_topology(VkPrimitiveTopology topology);
        void set_polygon_mode(VkPolygonMode mode);
        void set_cull_mode(VkCullModeFlags cull_mode, VkFrontFace front_face);
        void set_multisampling_none();
        void disable_blending();
        void set_color_attachment_format(VkFormat format);
        void set_depth_format(VkFormat format);
        void disable_depthtest();

        void clear();

        VkPipeline build_pipeline(VkDevice device);

    };

}