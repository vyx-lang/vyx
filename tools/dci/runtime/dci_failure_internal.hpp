#pragma once

namespace dci_failure_detail {

struct Owner {
    void (*copy_object)(void* dest, void* src);
    void (*destroy_object)(void* obj);
};

void remember(const void* payload, Owner owner);
Owner take(const void* payload);
Owner peek(const void* payload);

void* aligned_alloc_bytes(unsigned long long size, unsigned long long align);
void aligned_free_bytes(void* pointer);

}  // namespace dci_failure_detail
