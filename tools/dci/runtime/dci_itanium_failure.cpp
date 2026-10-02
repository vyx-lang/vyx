#include "dci_itanium_failure.hpp"
#include "dci_failure_internal.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <typeinfo>
#include <unordered_map>

#include <cxxabi.h>
#include <cstdlib>

#if defined(__ARM_EABI__) && !defined(__ARM_DWARF_EXCEPTIONS__)
#error "dci_itanium_failure.cpp covers DWARF Itanium EH, not ARM EHABI tables"
#endif

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__) || defined(__linux__)
#include <malloc.h>
#endif

namespace {

constexpr uint32_t kMaxPayload = 1u << 20;

struct DciEhGlobals {
    void* caughtExceptions;
    unsigned int uncaughtExceptions;
};

// Itanium C++ ABI §2.2.2 __cxa_exception (DWARF). LLVM libc++abi prepends
// referenceCount; GNU keeps that in __cxa_refcounted_exception.
struct DciCxaExceptionAbi {
    const std::type_info* exceptionType;
    void (*exceptionDestructor)(void*);
    void (*unexpectedHandler)();
    void (*terminateHandler)();
    DciCxaExceptionAbi* nextException;
    int handlerCount;
    int handlerSwitchValue;
    const char* actionRecord;
    const char* languageSpecificData;
    void* catchTemp;
    void* adjustedPtr;
    std::uint64_t unwindHeader[4];
};

struct DciCxaExceptionLlvm {
    std::size_t referenceCount;
    DciCxaExceptionAbi abi;
};

struct CopyCtx {
    std::exception_ptr ep;
    uint32_t size;
};

std::mutex g_ctx_mu;
std::unordered_map<const void*, CopyCtx> g_ctx;

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
    if (dest != nullptr && src != nullptr && ctx.size > 0) {
        std::memcpy(dest, src, ctx.size);
    }
    CopyCtx dest_ctx = ctx;
    dest_ctx.ep = ctx.ep;
    store_ctx(dest, std::move(dest_ctx));
}

void destroy_object_generic(void* obj) {
    CopyCtx ctx = load_ctx(obj);
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

const char* skip_itanium_qualifiers(const char* name) {
    while (name != nullptr && (*name == 'K' || *name == 'V' || *name == 'r')) {
        ++name;
    }
    return name;
}

bool fundamental_size(const char* name, uint32_t* size, uint32_t* align) {
    name = skip_itanium_qualifiers(name);
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    if (name[0] == 'P' || name[0] == 'R' || name[0] == 'O') {
        *size = static_cast<uint32_t>(sizeof(void*));
        *align = static_cast<uint32_t>(alignof(void*));
        return true;
    }
    if (name[0] == 'D' && name[1] == 'n') {
        *size = static_cast<uint32_t>(sizeof(void*));
        *align = static_cast<uint32_t>(alignof(void*));
        return true;
    }
    switch (name[0]) {
    case 'b':
    case 'c':
    case 'a':
    case 'h':
        *size = 1;
        *align = 1;
        return name[1] == '\0';
    case 's':
    case 't':
        *size = 2;
        *align = 2;
        return name[1] == '\0';
    case 'i':
    case 'j':
        *size = 4;
        *align = 4;
        return name[1] == '\0';
    case 'l':
    case 'm':
        *size = static_cast<uint32_t>(sizeof(long));
        *align = static_cast<uint32_t>(alignof(long));
        return name[1] == '\0';
    case 'x':
    case 'y':
        *size = 8;
        *align = 8;
        return name[1] == '\0';
    case 'n':
    case 'o':
        *size = 16;
        *align = 16;
        return name[1] == '\0';
    case 'f':
        *size = 4;
        *align = 4;
        return name[1] == '\0';
    case 'd':
        *size = 8;
        *align = 8;
        return name[1] == '\0';
    case 'e':
        *size = static_cast<uint32_t>(sizeof(long double));
        *align = static_cast<uint32_t>(alignof(long double));
        return name[1] == '\0';
    case 'w':
        *size = static_cast<uint32_t>(sizeof(wchar_t));
        *align = static_cast<uint32_t>(alignof(wchar_t));
        return name[1] == '\0';
    default:
        return false;
    }
}

size_t usable_bytes(void* pointer) {
    if (pointer == nullptr) {
        return 0;
    }
#if defined(__APPLE__)
    return malloc_size(pointer);
#elif defined(__GLIBC__) || defined(__linux__)
    return malloc_usable_size(pointer);
#else
    (void)pointer;
    return 0;
#endif
}

bool locate_header(
    const std::type_info* ti,
    DciCxaExceptionAbi** abi_out,
    void** object_out,
    void** header_out
) {
    *abi_out = nullptr;
    *object_out = nullptr;
    *header_out = nullptr;
    if (ti == nullptr) {
        return false;
    }
    auto* globals = reinterpret_cast<DciEhGlobals*>(__cxxabiv1::__cxa_get_globals());
    if (globals == nullptr || globals->caughtExceptions == nullptr) {
        return false;
    }
    char* base = static_cast<char*>(globals->caughtExceptions);
    auto* as_abi = reinterpret_cast<DciCxaExceptionAbi*>(base);
    DciCxaExceptionAbi* abi = nullptr;
    if (as_abi->exceptionType == ti) {
        abi = as_abi;
    } else {
        auto* as_llvm = reinterpret_cast<DciCxaExceptionLlvm*>(base);
        if (as_llvm->abi.exceptionType != ti) {
            return false;
        }
        abi = &as_llvm->abi;
        base = reinterpret_cast<char*>(as_llvm);
    }
    void* object = abi->adjustedPtr;
    if (object == nullptr) {
        object = abi + 1;
    }
    *abi_out = abi;
    *object_out = object;
    *header_out = base;
    return true;
}

uint32_t object_size(
    const std::type_info* ti,
    void* object,
    void* header
) {
    uint32_t size = 0;
    uint32_t align = 0;
    if (ti != nullptr && fundamental_size(ti->name(), &size, &align)) {
        return size;
    }
    char* thrown = static_cast<char*>(object);
    char* base = static_cast<char*>(header);
    if (thrown == nullptr || base == nullptr) {
        return 0;
    }
    const size_t prefixes[] = {16, 8, 0};
    for (size_t prefix : prefixes) {
        char* alloc = base - prefix;
        size_t usable = usable_bytes(alloc);
        if (usable == 0 || usable > kMaxPayload || thrown < alloc) {
            continue;
        }
        size_t n = usable - static_cast<size_t>(thrown - alloc);
        if (n > 0 && n <= kMaxPayload) {
            return static_cast<uint32_t>(n);
        }
    }
    return 0;
}

bool store_payload(DciFailure* out, const void* src, uint32_t size, uint32_t align) {
    if (src == nullptr || size == 0 || size > kMaxPayload) {
        return false;
    }
    uint64_t payload_align = align_for_size(align != 0 ? align : size);
    void* payload = dci_failure_detail::aligned_alloc_bytes(size, payload_align);
    if (payload == nullptr) {
        return false;
    }
    std::memcpy(payload, src, size);
    CopyCtx ctx{};
    ctx.ep = std::current_exception();
    ctx.size = size;
    store_ctx(payload, std::move(ctx));
    dci_failure_detail::remember(
        payload, dci_failure_detail::Owner{&copy_object_generic, &destroy_object_generic});
    out->payload = static_cast<uint8_t*>(payload);
    out->payload_size = size;
    out->payload_align = payload_align;
    return true;
}

template <typename T>
bool capture_arithmetic(DciFailure* out) {
    try {
        throw;
    } catch (T value) {
        return store_payload(
            out,
            &value,
            static_cast<uint32_t>(sizeof(T)),
            static_cast<uint32_t>(alignof(T)));
    } catch (...) {
        return false;
    }
}

bool capture_arithmetic_chain(DciFailure* out) {
    return capture_arithmetic<bool>(out)
        || capture_arithmetic<char>(out)
        || capture_arithmetic<signed char>(out)
        || capture_arithmetic<unsigned char>(out)
        || capture_arithmetic<short>(out)
        || capture_arithmetic<unsigned short>(out)
        || capture_arithmetic<int>(out)
        || capture_arithmetic<unsigned>(out)
        || capture_arithmetic<long>(out)
        || capture_arithmetic<unsigned long>(out)
        || capture_arithmetic<long long>(out)
        || capture_arithmetic<unsigned long long>(out)
        || capture_arithmetic<float>(out)
        || capture_arithmetic<double>(out)
        || capture_arithmetic<long double>(out)
        || capture_arithmetic<wchar_t>(out)
        || capture_arithmetic<const char*>(out)
        || capture_arithmetic<void*>(out);
}

const char* capture_exception_message() {
    try {
        throw;
    } catch (const std::exception& ex) {
        return ex.what();
    } catch (...) {
        return nullptr;
    }
}

}  // namespace

extern "C" int dci_itanium_capture_current(DciFailure* out) {
    if (out == nullptr) {
        return 0;
    }
    dci_failure_clear(out);

    const std::type_info* ti = __cxxabiv1::__cxa_current_exception_type();
    if (ti == nullptr) {
        return 0;
    }

    out->producer_tag = DCI_FAILURE_TAG_ITANIUM_CXX;
    out->type_identity = dup_cstr(ti->name());
    out->type_identity_len = out->type_identity != nullptr ? std::strlen(out->type_identity) : 0;

    DciCxaExceptionAbi* abi = nullptr;
    void* object = nullptr;
    void* alloc = nullptr;
    uint32_t header_size = 0;
    uint32_t header_align = 0;
    if (locate_header(ti, &abi, &object, &alloc) && object != nullptr) {
        uint32_t fund = 0;
        if (fundamental_size(ti->name(), &fund, &header_align) && fund != 0) {
            header_size = fund;
        } else {
            header_size = object_size(ti, object, alloc);
        }
    }

    void* snapshot = nullptr;
    if (object != nullptr && header_size != 0) {
        snapshot = std::malloc(header_size);
        if (snapshot != nullptr) {
            std::memcpy(snapshot, object, header_size);
        }
    }

    const bool exact = capture_arithmetic_chain(out);
    const char* message = capture_exception_message();
    out->message = dup_cstr(message);
    out->message_len = out->message != nullptr ? std::strlen(out->message) : 0;
    if (!exact && snapshot != nullptr && header_size != 0 && out->payload == nullptr) {
        store_payload(
            out,
            snapshot,
            header_size,
            header_align != 0 ? header_align : header_size);
    }
    std::free(snapshot);
    if (exact) {
        return 1;
    }
    if (out->payload == nullptr) {
        uint8_t token = 0;
        store_payload(out, &token, 1, 1);
    }
    return 1;
}
