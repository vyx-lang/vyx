#include <stdio.h>
#include <stddef.h>
#include "E:/Dev/C++/Cacao/include/CacaoC.h"

int main() {
    printf("sizeof GraphicsPipelineCreateInfo=%zu\n", sizeof(CacaoGraphicsPipelineCreateInfo));
    printf("  shaders=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, shaders));
    printf("  shader_count=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, shader_count));
    printf("  vertex_bindings=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, vertex_bindings));
    printf("  vertex_binding_count=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, vertex_binding_count));
    printf("  vertex_attributes=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, vertex_attributes));
    printf("  vertex_attribute_count=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, vertex_attribute_count));
    printf("  topology=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, topology));
    printf("  primitive_restart_enable=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, primitive_restart_enable));
    printf("  polygon_mode=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, polygon_mode));
    printf("  cull_mode=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, cull_mode));
    printf("  front_face=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, front_face));
    printf("  line_width=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, line_width));
    printf("  depth_test_enable=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, depth_test_enable));
    printf("  depth_write_enable=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, depth_write_enable));
    printf("  depth_compare_op=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, depth_compare_op));
    printf("  stencil_test_enable=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, stencil_test_enable));
    printf("  stencil_front=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, stencil_front));
    printf("  stencil_back=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, stencil_back));
    printf("  stencil_read_mask=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, stencil_read_mask));
    printf("  stencil_write_mask=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, stencil_write_mask));
    printf("  color_blend_attachments=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, color_blend_attachments));
    printf("  color_blend_attachment_count=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, color_blend_attachment_count));
    printf("  color_attachment_formats=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, color_attachment_formats));
    printf("  color_attachment_format_count=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, color_attachment_format_count));
    printf("  depth_stencil_format=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, depth_stencil_format));
    printf("  sample_count=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, sample_count));
    printf("  layout=%zu\n", offsetof(CacaoGraphicsPipelineCreateInfo, layout));
    printf("sizeof VertexInputAttribute=%zu\n", sizeof(CacaoVertexInputAttribute));
    printf("  semantic_name=%zu semantic_index=%zu\n",
           offsetof(CacaoVertexInputAttribute, semantic_name),
           offsetof(CacaoVertexInputAttribute, semantic_index));
    printf("sizeof ColorBlendAttachmentState=%zu\n", sizeof(CacaoColorBlendAttachmentState));
    printf("sizeof StencilOpState=%zu\n", sizeof(CacaoStencilOpState));
    printf("sizeof SurfaceTransform=%zu\n", sizeof(CacaoSurfaceTransform));
    return 0;
}
