#include "dci_msvc_failure.hpp"
#include "dci_failure_internal.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <new>
#include <unordered_map>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" void** __cdecl __current_exception();

namespace {

constexpr uint32_t kCppExceptionCode = 0xE06D7363u;
constexpr uint32_t kCtIsSimpleType = 1u;
constexpr uint32_t kCtHasVirtualBase = 4u;
constexpr int32_t kNoVirtualBase = -1;
constexpr int32_t kMaxCatchable = 256;
constexpr uint32_t kMaxPayload = 1u << 20;

struct ThrowInfo {
    uint32_t attributes;
    int32_t unwind_rva;
    int32_t forward_compat_rva;
    int32_t catchable_type_array_rva;
};

struct CatchableType {
    uint32_t properties;
    int32_t type_rva;
    int32_t mdisp;
    int32_t pdisp;
    int32_t vdisp;
    int32_t size_or_offset;
    int32_t copy_rva;
};

struct CatchableTypeArray {
    int32_t count;
    int32_t rvas[1];
};

struct TypeDescriptor {
    const void* vftable;
    void* spare;
    char name[1];
};

struct CopyCtx {
    const void* image_base;
    int32_t copy_rva;
    int32_t dtor_rva;
    uint32_t properties;
    uint32_t size;
    std::exception_ptr ep;
};

std::mutex g_ctx_mu;
std::unordered_map<const void*, CopyCtx> g_ctx;

char* image_rva(const void* image_base, int32_t rva) {
    if (image_base == nullptr || rva == 0) {
        return nullptr;
    }
    return static_cast<char*>(const_cast<void*>(image_base)) + rva;
}

const void* module_base_from_address(const void* address) {
    if (address == nullptr) {
        return nullptr;
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address),
            &module)) {
        return nullptr;
    }
    return module;
}

void* apply_pmd(void* object, const CatchableType* ct) {
    if (object == nullptr || ct == nullptr) {
        return nullptr;
    }
    char* p = static_cast<char*>(object) + ct->mdisp;
    if (ct->pdisp != kNoVirtualBase) {
        char* vbtable = *reinterpret_cast<char**>(static_cast<char*>(object) + ct->pdisp);
        if (vbtable == nullptr) {
            return p;
        }
        p += *reinterpret_cast<int*>(vbtable + ct->vdisp);
    }
    return p;
}

char* dup_cstr(const char* s) {
    if (s == nullptr) {
        return nullptr;
    }
    size_t n = std::strlen(s);
    char* out = static_cast<char*>(std::malloc(n + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, s, n + 1);
    return out;
}

using MemberCopy2 = void* (*)(void* dest, void* src);
using MemberCopy3 = void* (*)(void* dest, void* src, int not_most_derived);
using MemberDtor = void (*)(void* obj);

void call_copy(void* dest, void* src, const CopyCtx& ctx) {
    if (dest == nullptr || src == nullptr) {
        return;
    }
    if ((ctx.properties & kCtIsSimpleType) != 0 || ctx.copy_rva == 0) {
        std::memcpy(dest, src, ctx.size);
        return;
    }
    char* fn = image_rva(ctx.image_base, ctx.copy_rva);
    if (fn == nullptr) {
        std::memcpy(dest, src, ctx.size);
        return;
    }
    if ((ctx.properties & kCtHasVirtualBase) != 0) {
        reinterpret_cast<MemberCopy3>(fn)(dest, src, 1);
        return;
    }
    reinterpret_cast<MemberCopy2>(fn)(dest, src);
}

void store_ctx(const void* payload, CopyCtx ctx) {
    std::lock_guard<std::mutex> lock(g_ctx_mu);
    g_ctx[payload] = std::move(ctx);
}

CopyCtx load_ctx(const void* payload) {
    std::lock_guard<std::mutex> lock(g_ctx_mu);
    auto it = g_ctx.find(payload);
    if (it == g_ctx.end()) {
        return CopyCtx{};
    }
    return it->second;
}

void erase_ctx(const void* payload) {
    std::lock_guard<std::mutex> lock(g_ctx_mu);
    g_ctx.erase(payload);
}

void copy_object_generic(void* dest, void* src) {
    CopyCtx ctx = load_ctx(src);
    call_copy(dest, src, ctx);
    CopyCtx dest_ctx = ctx;
    dest_ctx.ep = ctx.ep;
    store_ctx(dest, std::move(dest_ctx));
}

void destroy_object_generic(void* obj) {
    CopyCtx ctx = load_ctx(obj);
    if (ctx.dtor_rva != 0 && ctx.image_base != nullptr
        && (ctx.properties & kCtIsSimpleType) == 0) {
        auto* dtor = reinterpret_cast<MemberDtor>(image_rva(ctx.image_base, ctx.dtor_rva));
        if (dtor != nullptr) {
            dtor(obj);
        }
    }
    ctx.ep = nullptr;
    erase_ctx(obj);
}

uint64_t align_for_size(uint32_t size) {
    if (size <= 1) {
        return 1;
    }
    if (size <= 2) {
        return 2;
    }
    if (size <= 4) {
        return 4;
    }
    if (size <= 8) {
        return 8;
    }
    return 16;
}

}  // namespace

extern "C" int dci_msvc_capture_current(DciFailure* out) {
    if (out == nullptr) {
        return 0;
    }
    dci_failure_clear(out);

    void** exception_slot = __current_exception();
    if (exception_slot == nullptr || *exception_slot == nullptr) {
        return 0;
    }
    auto* rec = static_cast<EXCEPTION_RECORD*>(*exception_slot);
    if (rec->ExceptionCode != kCppExceptionCode || rec->NumberParameters < 3) {
        return 0;
    }

    void* object = reinterpret_cast<void*>(rec->ExceptionInformation[1]);
    auto* throw_info = reinterpret_cast<ThrowInfo*>(rec->ExceptionInformation[2]);
    const void* image_base = nullptr;
    if (rec->NumberParameters >= 4 && rec->ExceptionInformation[3] != 0) {
        image_base = reinterpret_cast<const void*>(rec->ExceptionInformation[3]);
    }
    if (throw_info == nullptr || object == nullptr) {
        return 0;
    }
    if (image_base == nullptr) {
        image_base = module_base_from_address(throw_info);
    }
    if (image_base == nullptr) {
        return 0;
    }

    auto* array = reinterpret_cast<CatchableTypeArray*>(
        image_rva(image_base, throw_info->catchable_type_array_rva));
    if (array == nullptr || array->count <= 0 || array->count > kMaxCatchable) {
        return 0;
    }

    auto* primary = reinterpret_cast<CatchableType*>(image_rva(image_base, array->rvas[0]));
    if (primary == nullptr || primary->size_or_offset <= 0
        || static_cast<uint32_t>(primary->size_or_offset) > kMaxPayload) {
        return 0;
    }
    auto* type_desc = reinterpret_cast<TypeDescriptor*>(image_rva(image_base, primary->type_rva));
    const char* type_name = (type_desc != nullptr) ? type_desc->name : "";

    const uint32_t size = static_cast<uint32_t>(primary->size_or_offset);
    const uint64_t align = align_for_size(size);
    void* payload = dci_failure_detail::aligned_alloc_bytes(size, align);
    if (payload == nullptr) {
        return 0;
    }

    CopyCtx ctx{};
    ctx.image_base = image_base;
    ctx.copy_rva = primary->copy_rva;
    ctx.dtor_rva = throw_info->unwind_rva;
    ctx.properties = primary->properties;
    ctx.size = size;
    ctx.ep = std::current_exception();

    void* source = apply_pmd(object, primary);
    call_copy(payload, source, ctx);
    store_ctx(payload, ctx);
    dci_failure_detail::remember(
        payload, dci_failure_detail::Owner{&copy_object_generic, &destroy_object_generic});

    const char* message = nullptr;
    for (int32_t i = 0; i < array->count; ++i) {
        auto* ct = reinterpret_cast<CatchableType*>(image_rva(image_base, array->rvas[i]));
        if (ct == nullptr) {
            continue;
        }
        auto* td = reinterpret_cast<TypeDescriptor*>(image_rva(image_base, ct->type_rva));
        if (td == nullptr || td->name[0] == '\0') {
            continue;
        }
        if (std::strcmp(td->name, ".?AVexception@std@@") != 0) {
            continue;
        }
        auto* ex = static_cast<std::exception*>(apply_pmd(object, ct));
        if (ex != nullptr) {
            message = ex->what();
        }
        break;
    }

    out->producer_tag = DCI_FAILURE_TAG_MSVC_CXX;
    out->payload = static_cast<uint8_t*>(payload);
    out->payload_size = size;
    out->payload_align = align;
    out->type_identity = dup_cstr(type_name);
    out->type_identity_len = out->type_identity != nullptr ? std::strlen(out->type_identity) : 0;
    out->message = dup_cstr(message);
    out->message_len = out->message != nullptr ? std::strlen(out->message) : 0;
    return 1;
}
