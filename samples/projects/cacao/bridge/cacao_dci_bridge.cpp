#include "cacao_dci_bridge.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <windows.h>

#include "Cacao.hpp"

namespace cacao_dci {
struct TriangleState {
    HWND window = nullptr;
    Cacao::Ref<Cacao::Instance> instance;
    Cacao::Ref<Cacao::Adapter> adapter;
    Cacao::Ref<Cacao::Surface> surface;
    Cacao::Ref<Cacao::Device> device;
    Cacao::Ref<Cacao::Swapchain> swapchain;
    Cacao::Ref<Cacao::Synchronization> sync;
    Cacao::Ref<Cacao::Queue> queue;
    Cacao::Ref<Cacao::Queue> present_queue;
    Cacao::Ref<Cacao::CommandBufferEncoder> command;
    Cacao::Ref<Cacao::Buffer> vertex_buffer;
    Cacao::Ref<Cacao::GraphicsPipeline> pipeline;
    uint32_t frame = 0;
    bool image_initialized = false;
    bool initialized = false;
};

TriangleApp::TriangleApp(uint32_t width, uint32_t height) noexcept : impl_(nullptr) {
    try {
        auto* state = new TriangleState{};
        WNDCLASSA wc{};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "VyxDciTriangleWindow";
        RegisterClassA(&wc);
        state->window = CreateWindowA(wc.lpszClassName, "Vyx DCI Triangle",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
            static_cast<int>(width), static_cast<int>(height), nullptr, nullptr, wc.hInstance, nullptr);
        if (!state->window) { delete state; return; }
        Cacao::InstanceCreateInfo info{};
        info.type = Cacao::BackendType::DirectX12;
        info.applicationName = "Vyx DCI Triangle";
        info.enabledFeatures = {Cacao::InstanceFeature::Surface};
        state->instance = Cacao::Instance::Create(info);
        if (!state->instance) { delete state; return; }
        auto adapters = state->instance->EnumerateAdapters();
        if (adapters.empty()) { delete state; return; }
        state->adapter = adapters.front();
        Cacao::NativeWindowHandle native{};
        native.hWnd = state->window;
        native.hInst = wc.hInstance;
        state->surface = state->instance->CreateSurface(native);
        Cacao::DeviceCreateInfo device_info{};
        device_info.QueueRequests = {{Cacao::QueueType::Graphics, 1, 1.0f}};
        device_info.CompatibleSurface = state->surface;
        state->device = state->adapter->CreateDevice(device_info);
        auto caps = state->surface->GetCapabilities(state->adapter);
        state->swapchain = state->device->CreateSwapchain(Cacao::SwapchainBuilder()
            .SetExtent(caps.currentExtent).SetFormat(Cacao::Format::BGRA8_UNORM)
            .SetColorSpace(Cacao::ColorSpace::SRGB_NONLINEAR).SetPresentMode(Cacao::PresentMode::Fifo)
            .SetMinImageCount(caps.minImageCount + 1).SetSurface(state->surface).Build());
        const auto frames = std::max(1u, state->swapchain->GetImageCount() - 1);
        state->sync = state->device->CreateSynchronization(frames);
        state->queue = state->device->GetQueue(Cacao::QueueType::Graphics, 0);
        state->present_queue = state->surface->GetPresentQueue(state->device);
        state->command = state->device->CreateCommandBufferEncoder();
        struct Vertex { float pos[2]; float color[3]; } verts[] = {
            {{0.0f,0.5f},{1,0,0}}, {{-0.5f,-0.5f},{0,1,0}}, {{0.5f,-0.5f},{0,0,1}}};
        state->vertex_buffer = state->device->CreateBuffer(Cacao::BufferBuilder().SetSize(sizeof(verts))
            .SetUsage(Cacao::BufferUsageFlags::VertexBuffer).SetMemoryUsage(Cacao::BufferMemoryUsage::CpuToGpu).Build());
        std::memcpy(state->vertex_buffer->Map(), verts, sizeof(verts)); state->vertex_buffer->Unmap();
        auto compiler = state->instance->CreateShaderCompiler();
        Cacao::ShaderCreateInfo shader{}; shader.SourcePath = "target/hello_triangle.slang"; shader.EntryPoint = "mainVS"; shader.Stage = Cacao::ShaderStage::Vertex;
        auto vs = compiler->CompileOrLoad(state->device, shader); shader.EntryPoint = "mainPS"; shader.Stage = Cacao::ShaderStage::Fragment;
        auto ps = compiler->CompileOrLoad(state->device, shader);
        auto layout = state->device->CreatePipelineLayout(Cacao::PipelineLayoutBuilder().Build());
        state->pipeline = state->device->CreateGraphicsPipeline(Cacao::GraphicsPipelineBuilder().SetShaders({vs, ps})
            .AddVertexBinding(0, sizeof(Vertex)).AddVertexAttribute(0,0,Cacao::Format::RG32_FLOAT,0,"POSITION",0)
            .AddVertexAttribute(1,0,Cacao::Format::RGB32_FLOAT,8,"COLOR",0).SetTopology(Cacao::PrimitiveTopology::TriangleList)
            .SetCullMode(Cacao::CullMode::None).AddColorAttachmentDefault(false).AddColorFormat(state->swapchain->GetFormat()).SetLayout(layout).Build());
        if (!state->surface || !state->device || !state->swapchain || !state->pipeline || !state->present_queue) { delete state; return; }
        state->initialized = true;
        impl_ = state;
    } catch (...) {}
}

TriangleApp::~TriangleApp() { close(); }

bool TriangleApp::ready() const noexcept {
    const auto* state = static_cast<const TriangleState*>(impl_);
    return state != nullptr && state->initialized;
}

int32_t TriangleApp::draw_frame() noexcept {
    try {
        auto* s = static_cast<TriangleState*>(impl_); if (!s || !s->initialized) return -1;
        s->sync->WaitForFrame(0); int image = 0; if (s->swapchain->AcquireNextImage(s->sync, 0, image) != Cacao::Result::Success) { std::fprintf(stderr, "triangle: acquire failed\n"); return -2; }
        s->sync->ResetFrameFence(0); auto back = s->swapchain->GetBackBuffer(image); back->CreateDefaultViewIfNeeded();
        s->command->Reset(); s->command->Begin({true});
        s->command->TransitionImage(back, s->image_initialized ? Cacao::ImageTransition::PresentToColorAttachment : Cacao::ImageTransition::UndefinedToColorAttachment);
        s->image_initialized = true;
        Cacao::RenderingAttachmentInfo att{}; att.Texture = back; att.LoadOp = Cacao::AttachmentLoadOp::Clear; att.ClearValue = Cacao::ClearValue::ColorFloat(0.1f,0.1f,0.12f,1);
        Cacao::RenderingInfo ri{}; ri.RenderArea = {0,0,s->swapchain->GetExtent().width,s->swapchain->GetExtent().height}; ri.ColorAttachments = {att};
        s->command->BeginRendering(ri); s->command->SetViewport({0,0,(float)s->swapchain->GetExtent().width,(float)s->swapchain->GetExtent().height,0,1});
        s->command->SetScissor({0,0,s->swapchain->GetExtent().width,s->swapchain->GetExtent().height}); s->command->BindGraphicsPipeline(s->pipeline); s->command->BindVertexBuffer(0,s->vertex_buffer,0); s->command->Draw(3,1,0,0);
        s->command->EndRendering(); s->command->TransitionImage(back,Cacao::ImageTransition::ColorAttachmentToPresent); s->command->End(); s->queue->Submit(s->command,s->sync,0); s->swapchain->Present(s->present_queue,s->sync,0); s->sync->WaitIdle(); return 0;
    } catch (...) { std::fprintf(stderr, "triangle: C++ exception\n"); return -3; }
}

int32_t TriangleApp::run_frames(uint32_t frame_count) noexcept {
    for (uint32_t i = 0; i < frame_count; ++i) {
        const auto result = draw_frame();
        if (result != 0) return result;
    }
    return 0;
}

int32_t TriangleApp::run() noexcept {
    MSG message{};
    while (true) {
        while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return 0;
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        const auto result = draw_frame();
        if (result != 0) return result;
    }
}

void TriangleApp::close() noexcept {
    // Cacao owns backend threads that outlive a present in this debug build.
    // Keep the state process-owned until its shutdown ordering is finalized.
    impl_ = nullptr;
}
}

namespace {

constexpr int32_t kOk = 0;
constexpr int32_t kInvalidArgument = -1;
constexpr int32_t kFailure = -2;
constexpr int32_t kOutOfRange = -3;
constexpr int32_t kBufferTooSmall = -4;

struct InstanceHandle {
    Cacao::Ref<Cacao::Instance> value;
};

struct AdapterHandle {
    Cacao::Ref<Cacao::Adapter> value;
};

template <typename F>
int32_t boundary(F&& fn) {
    try {
        return fn();
    } catch (...) {
        return kFailure;
    }
}

}

extern "C" uint32_t CACAO_DCI_CALL cacao_dci_bridge_abi_version(void) noexcept {
    return 1;
}

extern "C" cacao_dci::Instance* CACAO_DCI_CALL cacao_dci_instance_create(uint32_t backend_type,
                                                                            uint32_t app_version) {
    try {
        Cacao::InstanceCreateInfo info{};
        info.type = static_cast<Cacao::BackendType>(backend_type);
        info.applicationName = "Vyx DCI bridge";
        info.appVersion = app_version;
        auto instance = Cacao::Instance::Create(info);
        if (!instance) return nullptr;
        return reinterpret_cast<cacao_dci::Instance*>(new InstanceHandle{std::move(instance)});
    } catch (...) { return nullptr; }
}

extern "C" void CACAO_DCI_CALL cacao_dci_instance_destroy(cacao_dci::Instance* instance) {
    delete reinterpret_cast<InstanceHandle*>(instance);
}

extern "C" int32_t CACAO_DCI_CALL cacao_dci_instance_adapter_count(cacao_dci::Instance* instance,
                                                                      uint32_t* out_count) {
    if (instance == nullptr || out_count == nullptr) return kInvalidArgument;
    return boundary([&] {
        const auto adapters = reinterpret_cast<InstanceHandle*>(instance)->value->EnumerateAdapters();
        *out_count = static_cast<uint32_t>(adapters.size());
        return kOk;
    });
}

extern "C" cacao_dci::Adapter* CACAO_DCI_CALL cacao_dci_instance_adapter_at(cacao_dci::Instance* instance,
                                                                                uint32_t index) {
    if (instance == nullptr) return nullptr;
    try {
        const auto adapters = reinterpret_cast<InstanceHandle*>(instance)->value->EnumerateAdapters();
        if (index >= adapters.size() || !adapters[index]) return nullptr;
        return reinterpret_cast<cacao_dci::Adapter*>(new AdapterHandle{adapters[index]});
    } catch (...) { return nullptr; }
}

extern "C" void CACAO_DCI_CALL cacao_dci_adapter_destroy(cacao_dci::Adapter* adapter) {
    delete reinterpret_cast<AdapterHandle*>(adapter);
}

extern "C" int32_t CACAO_DCI_CALL cacao_dci_adapter_snapshot(cacao_dci::Adapter* adapter,
                                                                uint32_t* out_device_id,
                                                                uint32_t* out_vendor_id,
                                                                uint32_t* out_adapter_type,
                                                                uint64_t* out_dedicated_memory,
                                                                char* name_buffer,
                                                                uint32_t name_buffer_size,
                                                                uint32_t* out_name_size) {
    if (adapter == nullptr || out_device_id == nullptr || out_vendor_id == nullptr ||
        out_adapter_type == nullptr || out_dedicated_memory == nullptr || out_name_size == nullptr) {
        return kInvalidArgument;
    }
    return boundary([&] {
        const auto properties = reinterpret_cast<AdapterHandle*>(adapter)->value->GetProperties();
        *out_device_id = properties.deviceID;
        *out_vendor_id = properties.vendorID;
        *out_adapter_type = static_cast<uint32_t>(properties.type);
        *out_dedicated_memory = properties.dedicatedVideoMemory;
        *out_name_size = static_cast<uint32_t>(properties.name.size());
        if (name_buffer == nullptr || name_buffer_size == 0) return kOk;
        const auto count = std::min<size_t>(properties.name.size(), name_buffer_size - 1);
        std::memcpy(name_buffer, properties.name.data(), count);
        name_buffer[count] = '\0';
        return count == properties.name.size() ? kOk : kBufferTooSmall;
    });
}

extern "C" int32_t CACAO_DCI_CALL cacao_dci_adapter_graphics_queue_family(cacao_dci::Adapter* adapter,
                                                                              uint32_t* out_family) {
    if (adapter == nullptr || out_family == nullptr) return kInvalidArgument;
    return boundary([&] {
        *out_family = reinterpret_cast<AdapterHandle*>(adapter)->value->FindQueueFamilyIndex(Cacao::QueueType::Graphics);
        return kOk;
    });
}

extern "C" int32_t CACAO_DCI_CALL cacao_dci_adapter_limits(cacao_dci::Adapter* adapter,
                                                              uint32_t* out_max_texture_2d,
                                                              uint32_t* out_max_color_attachments) {
    if (adapter == nullptr || out_max_texture_2d == nullptr || out_max_color_attachments == nullptr) {
        return kInvalidArgument;
    }
    return boundary([&] {
        const auto limits = reinterpret_cast<AdapterHandle*>(adapter)->value->QueryLimits();
        *out_max_texture_2d = limits.maxTextureSize2D;
        *out_max_color_attachments = limits.maxColorAttachments;
        return kOk;
    });
}
