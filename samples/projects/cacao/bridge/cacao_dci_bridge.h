#pragma once

#include <stdint.h>

#if defined(_WIN32)
#define CACAO_DCI_API __declspec(dllexport)
#define CACAO_DCI_CALL __cdecl
#else
#define CACAO_DCI_API
#define CACAO_DCI_CALL
#endif

namespace cacao_dci {
struct Instance { uint8_t opaque; };
struct Adapter { uint8_t opaque; };

class CACAO_DCI_API TriangleApp {
public:
    TriangleApp(uint32_t width, uint32_t height) noexcept;
    ~TriangleApp();
    bool ready() const noexcept;
    int32_t draw_frame() noexcept;
    int32_t run_frames(uint32_t frame_count) noexcept;
    int32_t run() noexcept;
    void close() noexcept;
private:
    void* impl_;
};
}

extern "C" {

CACAO_DCI_API uint32_t CACAO_DCI_CALL cacao_dci_bridge_abi_version(void) noexcept;
CACAO_DCI_API cacao_dci::Instance* CACAO_DCI_CALL cacao_dci_instance_create(uint32_t backend_type, uint32_t app_version);
CACAO_DCI_API void CACAO_DCI_CALL cacao_dci_instance_destroy(cacao_dci::Instance* instance);
CACAO_DCI_API int32_t CACAO_DCI_CALL cacao_dci_instance_adapter_count(cacao_dci::Instance* instance, uint32_t* out_count);
CACAO_DCI_API cacao_dci::Adapter* CACAO_DCI_CALL cacao_dci_instance_adapter_at(cacao_dci::Instance* instance, uint32_t index);
CACAO_DCI_API void CACAO_DCI_CALL cacao_dci_adapter_destroy(cacao_dci::Adapter* adapter);
CACAO_DCI_API int32_t CACAO_DCI_CALL cacao_dci_adapter_snapshot(cacao_dci::Adapter* adapter, uint32_t* out_device_id, uint32_t* out_vendor_id, uint32_t* out_adapter_type, uint64_t* out_dedicated_memory, char* name_buffer, uint32_t name_buffer_size, uint32_t* out_name_size);
CACAO_DCI_API int32_t CACAO_DCI_CALL cacao_dci_adapter_graphics_queue_family(cacao_dci::Adapter* adapter, uint32_t* out_family);
CACAO_DCI_API int32_t CACAO_DCI_CALL cacao_dci_adapter_limits(cacao_dci::Adapter* adapter, uint32_t* out_max_texture_2d, uint32_t* out_max_color_attachments);

}

/* dci-ownership
{
  "cacao_dci_instance_create(uint32_t,uint32_t)": { "return": "owned" },
  "cacao_dci_instance_destroy(cacao_dci::Instance*)": { "parameters": { "0": "move" } },
  "cacao_dci_instance_adapter_count(cacao_dci::Instance*,uint32_t*)": { "parameters": { "0": "borrow", "1": "out" } },
  "cacao_dci_instance_adapter_at(cacao_dci::Instance*,uint32_t)": { "parameters": { "0": "borrow" }, "return": "owned" },
  "cacao_dci_adapter_destroy(cacao_dci::Adapter*)": { "parameters": { "0": "move" } },
  "cacao_dci_adapter_snapshot(cacao_dci::Adapter*,uint32_t*,uint32_t*,uint32_t*,uint64_t*,char*,uint32_t,uint32_t*)": { "parameters": { "0": "borrow", "1": "out", "2": "out", "3": "out", "4": "out", "5": "out", "7": "out" } },
  "cacao_dci_adapter_graphics_queue_family(cacao_dci::Adapter*,uint32_t*)": { "parameters": { "0": "borrow", "1": "out" } },
  "cacao_dci_adapter_limits(cacao_dci::Adapter*,uint32_t*,uint32_t*)": { "parameters": { "0": "borrow", "1": "out", "2": "out" } }
}
dci-ownership-end */
