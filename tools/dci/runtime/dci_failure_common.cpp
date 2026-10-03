#include "dci_failure_abi.h"
#include "dci_failure_internal.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

char* dup_bytes(const void* src, size_t n) {
    if (src == nullptr || n == 0) {
        return nullptr;
    }
    char* out = static_cast<char*>(std::malloc(n + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, src, n);
    out[n] = 0;
    return out;
}

}  // namespace

namespace dci_failure_detail {

void* aligned_alloc_bytes(unsigned long long size, unsigned long long align) {
    if (size == 0) {
        return nullptr;
    }
    size_t boundary = static_cast<size_t>(align);
    if (boundary < sizeof(void*)) {
        boundary = sizeof(void*);
    }
    size_t bytes = static_cast<size_t>(size);
#if defined(_WIN32)
    return _aligned_malloc(bytes, boundary);
#else
    void* pointer = nullptr;
    if (posix_memalign(&pointer, boundary, bytes) != 0) {
        return nullptr;
    }
    return pointer;
#endif
}

void aligned_free_bytes(void* pointer) {
    if (pointer == nullptr) {
        return;
    }
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

std::mutex g_mu;
std::unordered_map<const void*, Owner> g_owners;

void remember(const void* payload, Owner owner) {
    if (payload == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_owners[payload] = owner;
}

Owner take(const void* payload) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_owners.find(payload);
    if (it == g_owners.end()) {
        return Owner{nullptr, nullptr};
    }
    Owner owner = it->second;
    g_owners.erase(it);
    return owner;
}

Owner peek(const void* payload) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_owners.find(payload);
    if (it == g_owners.end()) {
        return Owner{nullptr, nullptr};
    }
    return it->second;
}

}  // namespace dci_failure_detail

extern "C" void dci_failure_clear(DciFailure* failure) {
    if (failure == nullptr) {
        return;
    }
    std::memset(failure, 0, sizeof(*failure));
}

extern "C" uint64_t dci_failure_sizeof(void) {
    return sizeof(DciFailure);
}

extern "C" void dci_failure_destroy(DciFailure* failure) {
    if (failure == nullptr) {
        return;
    }
    dci_failure_detail::Owner owner = dci_failure_detail::take(failure->payload);
    if (failure->payload != nullptr) {
        if (owner.destroy_object != nullptr) {
            owner.destroy_object(const_cast<uint8_t*>(failure->payload));
        }
        dci_failure_detail::aligned_free_bytes(const_cast<uint8_t*>(failure->payload));
    }
    std::free(const_cast<char*>(failure->type_identity));
    std::free(const_cast<char*>(failure->message));
    dci_failure_clear(failure);
}

extern "C" int dci_failure_copy(const DciFailure* src, DciFailure* dst) {
    if (dst == nullptr) {
        return 0;
    }
    dci_failure_clear(dst);
    if (src == nullptr) {
        return 1;
    }
    dst->producer_tag = src->producer_tag;
    dst->payload_size = src->payload_size;
    dst->payload_align = src->payload_align;
    dst->type_identity_len = src->type_identity_len;
    dst->message_len = src->message_len;
    dst->type_identity = dup_bytes(src->type_identity, static_cast<size_t>(src->type_identity_len));
    dst->message = dup_bytes(src->message, static_cast<size_t>(src->message_len));
    if (src->payload != nullptr && src->payload_size > 0) {
        void* copy = dci_failure_detail::aligned_alloc_bytes(src->payload_size, src->payload_align);
        if (copy == nullptr) {
            dci_failure_destroy(dst);
            return 0;
        }
        dci_failure_detail::Owner owner = dci_failure_detail::peek(src->payload);
        if (owner.copy_object != nullptr) {
            owner.copy_object(copy, const_cast<uint8_t*>(src->payload));
        } else {
            std::memcpy(copy, src->payload, static_cast<size_t>(src->payload_size));
        }
        dst->payload = static_cast<uint8_t*>(copy);
        dci_failure_detail::remember(dst->payload, owner);
    }
    if ((src->type_identity != nullptr && dst->type_identity == nullptr && src->type_identity_len > 0)
        || (src->message != nullptr && dst->message == nullptr && src->message_len > 0)) {
        dci_failure_destroy(dst);
        return 0;
    }
    return 1;
}
