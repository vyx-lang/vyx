#ifndef CACAO_C_API_H
#define CACAO_C_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
    #if defined(CACAO_BUILD_DLL)
        #define CACAO_C_API __declspec(dllexport)
    #else
        #define CACAO_C_API __declspec(dllimport)
    #endif
    #define CACAO_CALL __cdecl
#else
    #if defined(CACAO_BUILD_DLL)
        #define CACAO_C_API __attribute__((visibility("default")))
    #else
        #define CACAO_C_API
    #endif
    #define CACAO_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CacaoResult {
    CACAO_OK = 0,
    CACAO_STATUS_TIMEOUT = 1,
    CACAO_STATUS_NOT_READY = 2,
    CACAO_STATUS_SUBOPTIMAL = 3,
    CACAO_STATUS_OUT_OF_DATE = 4,
    CACAO_STATUS_DEVICE_LOST = 5,
    CACAO_STATUS_ERROR = 6,
    CACAO_ERROR_INVALID_ARGUMENT = -1,
    CACAO_ERROR_NULL_HANDLE = -2,
    CACAO_ERROR_EXCEPTION = -3,
    CACAO_ERROR_BACKEND = -4,
    CACAO_ERROR_INSUFFICIENT_BUFFER = -5,
    CACAO_ERROR_DEVICE_LOST = -6,
    CACAO_ERROR_UNKNOWN = -100
} CacaoResult;

typedef struct CacaoInstance_T* CacaoInstance;
typedef struct CacaoAdapter_T* CacaoAdapter;
typedef struct CacaoDevice_T* CacaoDevice;
typedef struct CacaoQueue_T* CacaoQueue;
typedef struct CacaoSurface_T* CacaoSurface;
typedef struct CacaoSwapchain_T* CacaoSwapchain;
typedef struct CacaoBuffer_T* CacaoBuffer;
typedef struct CacaoTexture_T* CacaoTexture;
typedef struct CacaoTextureView_T* CacaoTextureView;
typedef struct CacaoSampler_T* CacaoSampler;
typedef struct CacaoShaderCompiler_T* CacaoShaderCompiler;
typedef struct CacaoShaderModule_T* CacaoShaderModule;
typedef struct CacaoDescriptorSetLayout_T* CacaoDescriptorSetLayout;
typedef struct CacaoDescriptorPool_T* CacaoDescriptorPool;
typedef struct CacaoDescriptorSet_T* CacaoDescriptorSet;
typedef struct CacaoPipelineLayout_T* CacaoPipelineLayout;
typedef struct CacaoGraphicsPipeline_T* CacaoGraphicsPipeline;
typedef struct CacaoComputePipeline_T* CacaoComputePipeline;
typedef struct CacaoAccelerationStructure_T* CacaoAccelerationStructure;
typedef struct CacaoRayTracingPipeline_T* CacaoRayTracingPipeline;
typedef struct CacaoShaderBindingTable_T* CacaoShaderBindingTable;
typedef struct CacaoCommandEncoder_T* CacaoCommandEncoder;
typedef struct CacaoSynchronization_T* CacaoSynchronization;
typedef struct CacaoTimelineSemaphore_T* CacaoTimelineSemaphore;
typedef struct CacaoQueryPool_T* CacaoQueryPool;

typedef struct CacaoExtent2D {
    uint32_t width;
    uint32_t height;
} CacaoExtent2D;

typedef struct CacaoRect2D {
    int32_t offset_x;
    int32_t offset_y;
    uint32_t width;
    uint32_t height;
} CacaoRect2D;

typedef struct CacaoNativeWindowHandle {
    void* hwnd;
    void* hinstance;
    void* metal_layer;
    void* wayland_display;
    void* wayland_surface;
    void* xcb_connection;
    uint32_t xcb_window;
    void* x11_display;
    uint64_t x11_window;
    void* android_native_window;
} CacaoNativeWindowHandle;

typedef struct CacaoSurfaceTransform {
    uint32_t rotation;
    bool flip_horizontal;
    bool flip_vertical;
} CacaoSurfaceTransform;

typedef struct CacaoSurfaceCapabilities {
    uint32_t min_image_count;
    uint32_t max_image_count;
    CacaoExtent2D current_extent;
    CacaoExtent2D min_image_extent;
    CacaoExtent2D max_image_extent;
    CacaoSurfaceTransform current_transform;
} CacaoSurfaceCapabilities;

typedef struct CacaoInstanceCreateInfo {
    uint32_t backend_type;
    const char* application_name;
    uint32_t app_version;
    const uint32_t* enabled_features;
    uint32_t enabled_feature_count;
} CacaoInstanceCreateInfo;

typedef struct CacaoAdapterProperties {
    uint32_t device_id;
    uint32_t vendor_id;
    char name[256];
    uint32_t adapter_type;
    uint64_t dedicated_video_memory;
} CacaoAdapterProperties;

typedef struct CacaoQueueRequest {
    uint32_t type;
    uint32_t count;
    float priority;
} CacaoQueueRequest;

typedef struct CacaoDeviceCreateInfo {
    const uint32_t* enabled_features;
    uint32_t enabled_feature_count;
    const CacaoQueueRequest* queue_requests;
    uint32_t queue_request_count;
    CacaoSurface compatible_surface;
    void* next;
} CacaoDeviceCreateInfo;

typedef struct CacaoSwapchainCreateInfo {
    CacaoExtent2D extent;
    uint32_t format;
    uint32_t color_space;
    uint32_t present_mode;
    uint32_t min_image_count;
    CacaoSurfaceTransform pre_transform;
    uint32_t composite_alpha;
    uint32_t usage;
    bool clipped;
    CacaoSurface compatible_surface;
    uint32_t image_array_layers;
} CacaoSwapchainCreateInfo;

typedef struct CacaoBufferCreateInfo {
    uint64_t size;
    uint32_t usage;
    uint32_t memory_usage;
    const char* name;
    const void* initial_data;
} CacaoBufferCreateInfo;

typedef struct CacaoTextureCreateInfo {
    uint32_t type;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_layers;
    uint32_t mip_levels;
    uint32_t format;
    uint32_t usage;
    uint32_t initial_state;
    uint32_t sample_count;
    const char* name;
    void* initial_data;
} CacaoTextureCreateInfo;

typedef struct CacaoSamplerCreateInfo {
    uint32_t mag_filter;
    uint32_t min_filter;
    uint32_t mipmap_mode;
    uint32_t address_mode_u;
    uint32_t address_mode_v;
    uint32_t address_mode_w;
    float mip_lod_bias;
    float min_lod;
    float max_lod;
    bool anisotropy_enable;
    float max_anisotropy;
    bool compare_enable;
    uint32_t compare_op;
    uint32_t border_color;
    bool unnormalized_coordinates;
    const char* name;
} CacaoSamplerCreateInfo;

typedef struct CacaoShaderCreateInfo {
    const char* source_path;
    const char* entry_point;
    uint32_t stage;
    const char* profile;
} CacaoShaderCreateInfo;

typedef struct CacaoDescriptorSetLayoutBinding {
    uint32_t binding;
    uint32_t type;
    uint32_t count;
    uint32_t stage_flags;
} CacaoDescriptorSetLayoutBinding;

typedef struct CacaoDescriptorSetLayoutCreateInfo {
    const CacaoDescriptorSetLayoutBinding* bindings;
    uint32_t binding_count;
    bool support_bindless;
} CacaoDescriptorSetLayoutCreateInfo;

typedef struct CacaoDescriptorPoolSize {
    uint32_t type;
    uint32_t count;
} CacaoDescriptorPoolSize;

typedef struct CacaoDescriptorPoolCreateInfo {
    uint32_t max_sets;
    const CacaoDescriptorPoolSize* pool_sizes;
    uint32_t pool_size_count;
} CacaoDescriptorPoolCreateInfo;

typedef struct CacaoBufferWriteInfo {
    uint32_t binding;
    CacaoBuffer buffer;
    uint64_t offset;
    uint64_t stride;
    uint64_t size;
    uint32_t type;
    uint32_t array_element;
} CacaoBufferWriteInfo;

typedef struct CacaoTextureWriteInfo {
    uint32_t binding;
    CacaoTextureView texture_view;
    uint32_t layout;
    uint32_t type;
    CacaoSampler sampler;
    uint32_t array_element;
} CacaoTextureWriteInfo;

typedef struct CacaoSamplerWriteInfo {
    uint32_t binding;
    CacaoSampler sampler;
    uint32_t array_element;
} CacaoSamplerWriteInfo;

typedef struct CacaoPushConstantRange {
    uint32_t stage_flags;
    uint32_t offset;
    uint32_t size;
} CacaoPushConstantRange;

typedef struct CacaoPipelineLayoutCreateInfo {
    const CacaoDescriptorSetLayout* set_layouts;
    uint32_t set_layout_count;
    const CacaoPushConstantRange* push_constant_ranges;
    uint32_t push_constant_range_count;
} CacaoPipelineLayoutCreateInfo;

typedef struct CacaoVertexInputBinding {
    uint32_t binding;
    uint32_t stride;
    uint32_t input_rate;
} CacaoVertexInputBinding;

typedef struct CacaoVertexInputAttribute {
    uint32_t location;
    uint32_t binding;
    uint32_t format;
    uint32_t offset;
    const char* semantic_name;
    uint32_t semantic_index;
} CacaoVertexInputAttribute;

typedef struct CacaoColorBlendAttachmentState {
    bool blend_enable;
    uint32_t src_color_blend_factor;
    uint32_t dst_color_blend_factor;
    uint32_t color_blend_op;
    uint32_t src_alpha_blend_factor;
    uint32_t dst_alpha_blend_factor;
    uint32_t alpha_blend_op;
    uint32_t color_write_mask;
} CacaoColorBlendAttachmentState;

typedef struct CacaoStencilOpState {
    uint32_t fail_op;
    uint32_t pass_op;
    uint32_t depth_fail_op;
    uint32_t compare_op;
    uint32_t compare_mask;
    uint32_t write_mask;
    uint32_t reference;
} CacaoStencilOpState;

typedef struct CacaoGraphicsPipelineCreateInfo {
    const CacaoShaderModule* shaders;
    uint32_t shader_count;
    const CacaoVertexInputBinding* vertex_bindings;
    uint32_t vertex_binding_count;
    const CacaoVertexInputAttribute* vertex_attributes;
    uint32_t vertex_attribute_count;
    uint32_t topology;
    bool primitive_restart_enable;
    uint32_t polygon_mode;
    uint32_t cull_mode;
    uint32_t front_face;
    float line_width;
    bool depth_test_enable;
    bool depth_write_enable;
    uint32_t depth_compare_op;
    bool stencil_test_enable;
    CacaoStencilOpState stencil_front;
    CacaoStencilOpState stencil_back;
    uint32_t stencil_read_mask;
    uint32_t stencil_write_mask;
    const CacaoColorBlendAttachmentState* color_blend_attachments;
    uint32_t color_blend_attachment_count;
    const uint32_t* color_attachment_formats;
    uint32_t color_attachment_format_count;
    uint32_t depth_stencil_format;
    uint32_t sample_count;
    CacaoPipelineLayout layout;
} CacaoGraphicsPipelineCreateInfo;

typedef struct CacaoComputePipelineCreateInfo {
    CacaoShaderModule compute_shader;
    CacaoPipelineLayout layout;
} CacaoComputePipelineCreateInfo;

typedef struct CacaoClearValue {
    float color[4];
    float depth;
    uint32_t stencil;
} CacaoClearValue;

typedef struct CacaoRenderingAttachmentInfo {
    CacaoTexture texture;
    uint32_t load_op;
    uint32_t store_op;
    CacaoClearValue clear_value;
} CacaoRenderingAttachmentInfo;

typedef struct CacaoRenderingInfo {
    CacaoRect2D render_area;
    const CacaoRenderingAttachmentInfo* color_attachments;
    uint32_t color_attachment_count;
    const CacaoRenderingAttachmentInfo* depth_attachment;
    const CacaoRenderingAttachmentInfo* stencil_attachment;
    uint32_t layer_count;
} CacaoRenderingInfo;

typedef struct CacaoBufferImageCopy {
    uint64_t buffer_offset;
    uint32_t buffer_row_length;
    uint32_t buffer_image_height;
    uint32_t aspect_mask;
    uint32_t mip_level;
    uint32_t base_array_layer;
    uint32_t layer_count;
    int32_t image_offset_x;
    int32_t image_offset_y;
    int32_t image_offset_z;
    uint32_t image_extent_width;
    uint32_t image_extent_height;
    uint32_t image_extent_depth;
} CacaoBufferImageCopy;

typedef struct CacaoAccelerationStructureGeometryDesc {
    CacaoBuffer vertex_buffer;
    uint64_t vertex_offset;
    uint32_t vertex_stride;
    uint32_t vertex_count;
    uint32_t vertex_format;
    CacaoBuffer index_buffer;
    uint64_t index_offset;
    uint32_t index_count;
    uint32_t index_format;
    CacaoBuffer transform_buffer;
    uint64_t transform_offset;
    bool opaque;
} CacaoAccelerationStructureGeometryDesc;

typedef struct CacaoAccelerationStructureInstance {
    float transform[12];
    uint32_t instance_id;
    uint32_t mask;
    uint32_t shader_binding_table_offset;
    uint32_t flags;
    uint64_t acceleration_structure_address;
} CacaoAccelerationStructureInstance;

typedef struct CacaoAccelerationStructureCreateInfo {
    uint32_t type;
    const CacaoAccelerationStructureGeometryDesc* geometries;
    uint32_t geometry_count;
    const CacaoAccelerationStructureInstance* instances;
    uint32_t instance_count;
    bool allow_update;
    bool prefer_fast_trace;
} CacaoAccelerationStructureCreateInfo;

typedef struct CacaoRayTracingPipelineCreateInfo {
    const CacaoShaderModule* shaders;
    uint32_t shader_count;
    uint32_t max_recursion_depth;
    CacaoPipelineLayout layout;
} CacaoRayTracingPipelineCreateInfo;

typedef struct CacaoTextureBarrier {
    CacaoTexture texture;
    uint32_t old_state;
    uint32_t new_state;
    uint32_t base_mip_level;
    uint32_t level_count;
    uint32_t base_array_layer;
    uint32_t layer_count;
    uint32_t aspect_mask;
    uint32_t src_queue_family;
    uint32_t dst_queue_family;
} CacaoTextureBarrier;

typedef struct CacaoMemoryBarrier {
    uint32_t old_state;
    uint32_t new_state;
} CacaoMemoryBarrier;

typedef struct CacaoBufferBarrier {
    CacaoBuffer buffer;
    uint32_t old_state;
    uint32_t new_state;
    uint64_t offset;
    uint64_t size;
    uint32_t src_queue_family;
    uint32_t dst_queue_family;
} CacaoBufferBarrier;

typedef struct CacaoTextureViewDesc {
    uint32_t view_type;
    uint32_t format_override;
    uint32_t base_mip_level;
    uint32_t mip_level_count;
    uint32_t base_array_layer;
    uint32_t array_layer_count;
    uint32_t aspect;
    const char* name;
} CacaoTextureViewDesc;

typedef struct CacaoQueryPoolCreateInfo {
    uint32_t type;
    uint32_t count;
    uint32_t pipeline_statistics_flags;
} CacaoQueryPoolCreateInfo;

CACAO_C_API uint32_t CACAO_CALL cacao_get_abi_version(void);
CACAO_C_API const char* CACAO_CALL cacao_get_last_error(void);
CACAO_C_API void CACAO_CALL cacao_clear_last_error(void);

CACAO_C_API CacaoResult CACAO_CALL cacao_instance_create(const CacaoInstanceCreateInfo* info, CacaoInstance* out_instance);
CACAO_C_API void CACAO_CALL cacao_instance_release(CacaoInstance instance);
CACAO_C_API CacaoResult CACAO_CALL cacao_instance_get_backend_type(CacaoInstance instance, uint32_t* out_backend_type);
CACAO_C_API CacaoResult CACAO_CALL cacao_instance_enumerate_adapters(CacaoInstance instance, CacaoAdapter* out_adapters, uint32_t* inout_count);
CACAO_C_API CacaoResult CACAO_CALL cacao_instance_create_surface(CacaoInstance instance, const CacaoNativeWindowHandle* native_window, CacaoSurface* out_surface);
CACAO_C_API CacaoResult CACAO_CALL cacao_instance_create_shader_compiler(CacaoInstance instance, CacaoShaderCompiler* out_compiler);

CACAO_C_API void CACAO_CALL cacao_adapter_release(CacaoAdapter adapter);
CACAO_C_API CacaoResult CACAO_CALL cacao_adapter_get_properties(CacaoAdapter adapter, CacaoAdapterProperties* out_properties);
CACAO_C_API CacaoResult CACAO_CALL cacao_adapter_get_type(CacaoAdapter adapter, uint32_t* out_type);
CACAO_C_API CacaoResult CACAO_CALL cacao_adapter_is_feature_supported(CacaoAdapter adapter, uint32_t feature, bool* out_supported);
CACAO_C_API CacaoResult CACAO_CALL cacao_adapter_find_queue_family_index(CacaoAdapter adapter, uint32_t queue_type, uint32_t* out_index);
CACAO_C_API CacaoResult CACAO_CALL cacao_adapter_create_device(CacaoAdapter adapter, const CacaoDeviceCreateInfo* info, CacaoDevice* out_device);

CACAO_C_API void CACAO_CALL cacao_device_release(CacaoDevice device);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_get_queue(CacaoDevice device, uint32_t queue_type, uint32_t index, CacaoQueue* out_queue);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_swapchain(CacaoDevice device, const CacaoSwapchainCreateInfo* info, CacaoSwapchain* out_swapchain);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_synchronization(CacaoDevice device, uint32_t max_frames_in_flight, CacaoSynchronization* out_sync);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_timeline_semaphore(CacaoDevice device, uint64_t initial_value, CacaoTimelineSemaphore* out_semaphore);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_query_pool(CacaoDevice device, const CacaoQueryPoolCreateInfo* info, CacaoQueryPool* out_pool);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_buffer(CacaoDevice device, const CacaoBufferCreateInfo* info, CacaoBuffer* out_buffer);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_texture(CacaoDevice device, const CacaoTextureCreateInfo* info, CacaoTexture* out_texture);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_sampler(CacaoDevice device, const CacaoSamplerCreateInfo* info, CacaoSampler* out_sampler);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_descriptor_set_layout(CacaoDevice device, const CacaoDescriptorSetLayoutCreateInfo* info, CacaoDescriptorSetLayout* out_layout);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_descriptor_pool(CacaoDevice device, const CacaoDescriptorPoolCreateInfo* info, CacaoDescriptorPool* out_pool);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_pipeline_layout(CacaoDevice device, const CacaoPipelineLayoutCreateInfo* info, CacaoPipelineLayout* out_layout);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_graphics_pipeline(CacaoDevice device, const CacaoGraphicsPipelineCreateInfo* info, CacaoGraphicsPipeline* out_pipeline);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_compute_pipeline(CacaoDevice device, const CacaoComputePipelineCreateInfo* info, CacaoComputePipeline* out_pipeline);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_acceleration_structure(CacaoDevice device, const CacaoAccelerationStructureCreateInfo* info, CacaoAccelerationStructure* out_acceleration_structure);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_ray_tracing_pipeline(CacaoDevice device, const CacaoRayTracingPipelineCreateInfo* info, CacaoRayTracingPipeline* out_pipeline);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_shader_binding_table(CacaoDevice device, CacaoRayTracingPipeline pipeline, uint32_t ray_gen_count, uint32_t miss_count, uint32_t hit_group_count, uint32_t callable_count, CacaoShaderBindingTable* out_sbt);
CACAO_C_API CacaoResult CACAO_CALL cacao_device_create_command_encoder(CacaoDevice device, uint32_t command_buffer_type, CacaoCommandEncoder* out_encoder);

CACAO_C_API void CACAO_CALL cacao_queue_release(CacaoQueue queue);
CACAO_C_API CacaoResult CACAO_CALL cacao_queue_submit(CacaoQueue queue, CacaoCommandEncoder command, CacaoSynchronization sync, uint32_t frame_index);
CACAO_C_API CacaoResult CACAO_CALL cacao_queue_submit_simple(CacaoQueue queue, CacaoCommandEncoder command);
CACAO_C_API CacaoResult CACAO_CALL cacao_queue_wait_idle(CacaoQueue queue);

CACAO_C_API void CACAO_CALL cacao_surface_release(CacaoSurface surface);
CACAO_C_API CacaoResult CACAO_CALL cacao_surface_get_capabilities(CacaoSurface surface, CacaoAdapter adapter, CacaoSurfaceCapabilities* out_caps);

CACAO_C_API void CACAO_CALL cacao_swapchain_release(CacaoSwapchain swapchain);
CACAO_C_API CacaoResult CACAO_CALL cacao_swapchain_acquire_next_image(CacaoSwapchain swapchain, CacaoSynchronization sync, uint32_t frame_index, uint32_t* out_image_index);
CACAO_C_API CacaoResult CACAO_CALL cacao_swapchain_present(CacaoSwapchain swapchain, CacaoQueue queue, CacaoSynchronization sync, uint32_t frame_index);
CACAO_C_API CacaoResult CACAO_CALL cacao_swapchain_get_back_buffer(CacaoSwapchain swapchain, uint32_t index, CacaoTexture* out_texture);
CACAO_C_API CacaoResult CACAO_CALL cacao_swapchain_get_image_count(CacaoSwapchain swapchain, uint32_t* out_count);
CACAO_C_API CacaoResult CACAO_CALL cacao_swapchain_get_extent(CacaoSwapchain swapchain, CacaoExtent2D* out_extent);
CACAO_C_API CacaoResult CACAO_CALL cacao_swapchain_get_format(CacaoSwapchain swapchain, uint32_t* out_format);

CACAO_C_API void CACAO_CALL cacao_sync_release(CacaoSynchronization sync);
CACAO_C_API CacaoResult CACAO_CALL cacao_sync_wait_for_frame(CacaoSynchronization sync, uint32_t frame_index);
CACAO_C_API CacaoResult CACAO_CALL cacao_sync_reset_frame_fence(CacaoSynchronization sync, uint32_t frame_index);
CACAO_C_API CacaoResult CACAO_CALL cacao_sync_wait_idle(CacaoSynchronization sync);
CACAO_C_API CacaoResult CACAO_CALL cacao_sync_get_max_frames_in_flight(CacaoSynchronization sync, uint32_t* out_count);

CACAO_C_API void CACAO_CALL cacao_timeline_semaphore_release(CacaoTimelineSemaphore semaphore);
CACAO_C_API CacaoResult CACAO_CALL cacao_timeline_semaphore_signal(CacaoTimelineSemaphore semaphore, uint64_t value);
CACAO_C_API CacaoResult CACAO_CALL cacao_timeline_semaphore_wait(CacaoTimelineSemaphore semaphore, uint64_t value, uint64_t timeout_ns);
CACAO_C_API CacaoResult CACAO_CALL cacao_timeline_semaphore_get_value(CacaoTimelineSemaphore semaphore, uint64_t* out_value);

CACAO_C_API void CACAO_CALL cacao_query_pool_release(CacaoQueryPool pool);
CACAO_C_API CacaoResult CACAO_CALL cacao_query_pool_reset(CacaoQueryPool pool, uint32_t first_query, uint32_t count);
CACAO_C_API CacaoResult CACAO_CALL cacao_query_pool_get_results(CacaoQueryPool pool, uint32_t first_query, uint32_t query_count, uint64_t* out_results, bool wait);
CACAO_C_API CacaoResult CACAO_CALL cacao_query_pool_get_type(CacaoQueryPool pool, uint32_t* out_type);
CACAO_C_API CacaoResult CACAO_CALL cacao_query_pool_get_count(CacaoQueryPool pool, uint32_t* out_count);

CACAO_C_API void CACAO_CALL cacao_buffer_release(CacaoBuffer buffer);
CACAO_C_API CacaoResult CACAO_CALL cacao_buffer_map(CacaoBuffer buffer, void** out_data);
CACAO_C_API CacaoResult CACAO_CALL cacao_buffer_unmap(CacaoBuffer buffer);
CACAO_C_API CacaoResult CACAO_CALL cacao_buffer_flush(CacaoBuffer buffer, uint64_t offset, uint64_t size);
CACAO_C_API CacaoResult CACAO_CALL cacao_buffer_get_size(CacaoBuffer buffer, uint64_t* out_size);
CACAO_C_API CacaoResult CACAO_CALL cacao_buffer_get_usage(CacaoBuffer buffer, uint32_t* out_usage);
CACAO_C_API CacaoResult CACAO_CALL cacao_buffer_get_memory_usage(CacaoBuffer buffer, uint32_t* out_memory_usage);
CACAO_C_API CacaoResult CACAO_CALL cacao_buffer_get_device_address(CacaoBuffer buffer, uint64_t* out_address);
CACAO_C_API CacaoResult CACAO_CALL cacao_buffer_write(CacaoBuffer buffer, const void* data, uint64_t size, uint64_t offset);

CACAO_C_API void CACAO_CALL cacao_texture_release(CacaoTexture texture);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_get_width(CacaoTexture texture, uint32_t* out_width);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_get_height(CacaoTexture texture, uint32_t* out_height);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_get_depth(CacaoTexture texture, uint32_t* out_depth);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_get_mip_levels(CacaoTexture texture, uint32_t* out_mip_levels);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_get_array_layers(CacaoTexture texture, uint32_t* out_array_layers);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_get_format(CacaoTexture texture, uint32_t* out_format);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_get_type(CacaoTexture texture, uint32_t* out_type);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_get_sample_count(CacaoTexture texture, uint32_t* out_sample_count);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_get_usage(CacaoTexture texture, uint32_t* out_usage);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_get_current_state(CacaoTexture texture, uint32_t* out_state);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_create_view(CacaoTexture texture, const CacaoTextureViewDesc* desc, CacaoTextureView* out_view);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_create_default_view_if_needed(CacaoTexture texture);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_get_default_view(CacaoTexture texture, CacaoTextureView* out_view);

CACAO_C_API void CACAO_CALL cacao_texture_view_release(CacaoTextureView view);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_view_get_texture(CacaoTextureView view, CacaoTexture* out_texture);
CACAO_C_API CacaoResult CACAO_CALL cacao_texture_view_get_desc(CacaoTextureView view, CacaoTextureViewDesc* out_desc);
CACAO_C_API void CACAO_CALL cacao_sampler_release(CacaoSampler sampler);
CACAO_C_API void CACAO_CALL cacao_shader_compiler_release(CacaoShaderCompiler compiler);
CACAO_C_API CacaoResult CACAO_CALL cacao_shader_compiler_set_cache_directory(CacaoShaderCompiler compiler, const char* path);
CACAO_C_API CacaoResult CACAO_CALL cacao_shader_compiler_compile_or_load(CacaoShaderCompiler compiler, CacaoDevice device, const CacaoShaderCreateInfo* info, CacaoShaderModule* out_shader);
CACAO_C_API void CACAO_CALL cacao_shader_module_release(CacaoShaderModule shader);
CACAO_C_API void CACAO_CALL cacao_descriptor_set_layout_release(CacaoDescriptorSetLayout layout);
CACAO_C_API void CACAO_CALL cacao_descriptor_pool_release(CacaoDescriptorPool pool);
CACAO_C_API CacaoResult CACAO_CALL cacao_descriptor_pool_allocate_set(CacaoDescriptorPool pool, CacaoDescriptorSetLayout layout, CacaoDescriptorSet* out_set);
CACAO_C_API void CACAO_CALL cacao_descriptor_set_release(CacaoDescriptorSet set);
CACAO_C_API CacaoResult CACAO_CALL cacao_descriptor_set_write_buffer(CacaoDescriptorSet set, const CacaoBufferWriteInfo* info);
CACAO_C_API CacaoResult CACAO_CALL cacao_descriptor_set_write_texture(CacaoDescriptorSet set, const CacaoTextureWriteInfo* info);
CACAO_C_API CacaoResult CACAO_CALL cacao_descriptor_set_write_sampler(CacaoDescriptorSet set, const CacaoSamplerWriteInfo* info);
CACAO_C_API CacaoResult CACAO_CALL cacao_descriptor_set_write_acceleration_structure(CacaoDescriptorSet set, uint32_t binding, CacaoAccelerationStructure acceleration_structure, uint32_t type);
CACAO_C_API CacaoResult CACAO_CALL cacao_descriptor_set_update(CacaoDescriptorSet set);
CACAO_C_API void CACAO_CALL cacao_pipeline_layout_release(CacaoPipelineLayout layout);
CACAO_C_API void CACAO_CALL cacao_graphics_pipeline_release(CacaoGraphicsPipeline pipeline);
CACAO_C_API void CACAO_CALL cacao_compute_pipeline_release(CacaoComputePipeline pipeline);
CACAO_C_API void CACAO_CALL cacao_acceleration_structure_release(CacaoAccelerationStructure acceleration_structure);
CACAO_C_API CacaoResult CACAO_CALL cacao_acceleration_structure_get_device_address(CacaoAccelerationStructure acceleration_structure, uint64_t* out_address);
CACAO_C_API CacaoResult CACAO_CALL cacao_acceleration_structure_get_scratch_size(CacaoAccelerationStructure acceleration_structure, uint64_t* out_size);
CACAO_C_API void CACAO_CALL cacao_ray_tracing_pipeline_release(CacaoRayTracingPipeline pipeline);
CACAO_C_API void CACAO_CALL cacao_shader_binding_table_release(CacaoShaderBindingTable sbt);

CACAO_C_API void CACAO_CALL cacao_command_encoder_release(CacaoCommandEncoder command);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_reset(CacaoCommandEncoder command);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_begin(CacaoCommandEncoder command, bool one_time_submit, bool simultaneous_use);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_end(CacaoCommandEncoder command);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_begin_rendering(CacaoCommandEncoder command, const CacaoRenderingInfo* info);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_end_rendering(CacaoCommandEncoder command);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_set_viewport(CacaoCommandEncoder command, float x, float y, float width, float height, float min_depth, float max_depth);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_set_scissor(CacaoCommandEncoder command, const CacaoRect2D* rect);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_transition_image(CacaoCommandEncoder command, CacaoTexture texture, uint32_t transition);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_transition_buffer(CacaoCommandEncoder command, CacaoBuffer buffer, uint32_t transition, uint64_t offset, uint64_t size);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_copy_buffer(CacaoCommandEncoder command, CacaoBuffer src, CacaoBuffer dst, uint64_t src_offset, uint64_t dst_offset, uint64_t size);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_copy_buffer_to_image(CacaoCommandEncoder command, CacaoBuffer src, CacaoTexture dst, uint32_t dst_image_layout, const CacaoBufferImageCopy* regions, uint32_t region_count);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_build_acceleration_structure(CacaoCommandEncoder command, CacaoAccelerationStructure acceleration_structure);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_memory_barrier_fast(CacaoCommandEncoder command, uint32_t transition);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_pipeline_barrier(CacaoCommandEncoder command, uint32_t src_scope, uint32_t dst_scope, const CacaoMemoryBarrier* memory_barriers, uint32_t memory_barrier_count, const CacaoBufferBarrier* buffer_barriers, uint32_t buffer_barrier_count, const CacaoTextureBarrier* texture_barriers, uint32_t texture_barrier_count);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_pipeline_barrier_textures(CacaoCommandEncoder command, uint32_t src_scope, uint32_t dst_scope, const CacaoTextureBarrier* barriers, uint32_t barrier_count);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_copy_texture_2d(CacaoCommandEncoder command, CacaoTexture src, CacaoTexture dst);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_bind_graphics_pipeline(CacaoCommandEncoder command, CacaoGraphicsPipeline pipeline);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_bind_compute_pipeline(CacaoCommandEncoder command, CacaoComputePipeline pipeline);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_dispatch(CacaoCommandEncoder command, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_bind_compute_descriptor_sets(CacaoCommandEncoder command, CacaoComputePipeline pipeline, uint32_t first_set, const CacaoDescriptorSet* sets, uint32_t set_count);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_compute_push_constants(CacaoCommandEncoder command, CacaoComputePipeline pipeline, uint32_t stage_flags, uint32_t offset, uint32_t size, const void* data);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_dispatch_indirect(CacaoCommandEncoder command, CacaoBuffer arg_buffer, uint64_t offset);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_draw_indirect(CacaoCommandEncoder command, CacaoBuffer arg_buffer, uint64_t offset, uint32_t draw_count, uint32_t stride);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_draw_indexed_indirect(CacaoCommandEncoder command, CacaoBuffer arg_buffer, uint64_t offset, uint32_t draw_count, uint32_t stride);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_draw_indirect_count(CacaoCommandEncoder command, CacaoBuffer arg_buffer, uint64_t offset, CacaoBuffer count_buffer, uint64_t count_buffer_offset, uint32_t max_draw_count, uint32_t stride);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_draw_indexed_indirect_count(CacaoCommandEncoder command, CacaoBuffer arg_buffer, uint64_t offset, CacaoBuffer count_buffer, uint64_t count_buffer_offset, uint32_t max_draw_count, uint32_t stride);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_dispatch_mesh(CacaoCommandEncoder command, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_begin_debug_label(CacaoCommandEncoder command, const char* name, float r, float g, float b, float a);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_end_debug_label(CacaoCommandEncoder command);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_insert_debug_label(CacaoCommandEncoder command, const char* name, float r, float g, float b, float a);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_begin_query(CacaoCommandEncoder command, CacaoQueryPool pool, uint32_t query_index);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_end_query(CacaoCommandEncoder command, CacaoQueryPool pool, uint32_t query_index);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_write_timestamp(CacaoCommandEncoder command, CacaoQueryPool pool, uint32_t query_index);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_reset_query_pool(CacaoCommandEncoder command, CacaoQueryPool pool, uint32_t first_query, uint32_t count);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_bind_ray_tracing_pipeline(CacaoCommandEncoder command, CacaoRayTracingPipeline pipeline);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_bind_ray_tracing_descriptor_sets(CacaoCommandEncoder command, CacaoRayTracingPipeline pipeline, uint32_t first_set, const CacaoDescriptorSet* sets, uint32_t set_count);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_trace_rays(CacaoCommandEncoder command, CacaoShaderBindingTable sbt, uint32_t width, uint32_t height, uint32_t depth);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_bind_vertex_buffer(CacaoCommandEncoder command, uint32_t binding, CacaoBuffer buffer, uint64_t offset);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_bind_index_buffer(CacaoCommandEncoder command, CacaoBuffer buffer, uint64_t offset, uint32_t index_type);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_bind_descriptor_sets(CacaoCommandEncoder command, CacaoGraphicsPipeline pipeline, uint32_t first_set, const CacaoDescriptorSet* sets, uint32_t set_count);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_push_constants(CacaoCommandEncoder command, CacaoGraphicsPipeline pipeline, uint32_t stage_flags, uint32_t offset, uint32_t size, const void* data);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_draw(CacaoCommandEncoder command, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance);
CACAO_C_API CacaoResult CACAO_CALL cacao_cmd_draw_indexed(CacaoCommandEncoder command, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance);

#ifdef __cplusplus
}
#endif

#endif
