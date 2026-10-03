#ifndef VYX_POINTER_RT_ABI
#  error "VYX_POINTER_RT_ABI must name the runtime export attribute"
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <new>
#include "../include/vyx_pointer_region_abi.h"

namespace {

enum class VyxPointerCapability : std::uintptr_t {
    Compat = 0,
    Shared = 1,
    Mutable = 2,
};

using VyxPointerRegion = vyx_pointer_region_abi::Region;

[[noreturn]] void vyx_pointer_fail(const char* message) noexcept;

struct VyxPointerRegionRegistry final {
    std::mutex mutex;
    VyxPointerRegion* head = nullptr;
    struct RawAllocation {
        void* base = nullptr;
        std::uint64_t bytes = 0;
        bool alive = false;
        RawAllocation* next = nullptr;
    }* allocations = nullptr;
};

VyxPointerRegionRegistry& vyx_pointer_region_registry() {
    // The registry is process-lifetime state. Keeping it intentionally alive
    // avoids running allocator and mutex teardown from a shared runtime DLL
    // after the generated module's CRT has begun shutting down. The operating
    // system reclaims registry metadata when the process terminates.
    static VyxPointerRegionRegistry* registry = []() noexcept {
        void* storage = std::malloc(sizeof(VyxPointerRegionRegistry));
        if (!storage) {
            vyx_pointer_fail("vyx: cannot allocate pointer region registry");
        }
        return ::new (storage) VyxPointerRegionRegistry();
    }();
    return *registry;
}

[[noreturn]] void vyx_pointer_fail(const char* message) noexcept {
    std::fputs(message, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

VyxPointerRegion* vyx_pointer_region_from_metadata(void* metadata) noexcept {
    const auto bits = reinterpret_cast<std::uintptr_t>(metadata);
    return reinterpret_cast<VyxPointerRegion*>(bits & ~std::uintptr_t{3});
}

VyxPointerCapability vyx_pointer_capability(void* metadata) noexcept {
    return static_cast<VyxPointerCapability>(
        reinterpret_cast<std::uintptr_t>(metadata) & std::uintptr_t{3});
}

void* vyx_pointer_tag_metadata(VyxPointerRegion* region,
                               VyxPointerCapability capability) noexcept {
    if (!region) {
        return nullptr;
    }
    const auto bits = reinterpret_cast<std::uintptr_t>(region);
    return reinterpret_cast<void*>(bits | static_cast<std::uintptr_t>(capability));
}

bool vyx_pointer_address_in_span(void* address,
                                 void* base,
                                 std::uint64_t bytes) noexcept {
    if (!address || !base || bytes == 0) {
        return false;
    }
    const auto current = reinterpret_cast<std::uintptr_t>(address);
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    return current >= start && static_cast<std::uint64_t>(current - start) < bytes;
}

std::uint64_t vyx_pointer_registered_remaining(void* address) noexcept {
    if (!address) {
        return 0;
    }
    auto& registry = vyx_pointer_region_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    std::uint64_t best = 0;
    for (auto* allocation = registry.allocations;
         allocation;
         allocation = allocation->next) {
        if (!allocation->alive
            || !vyx_pointer_address_in_span(address, allocation->base, allocation->bytes)) {
            continue;
        }
        const auto remaining = allocation->bytes
            - static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address)
                                         - reinterpret_cast<std::uintptr_t>(allocation->base));
        if (allocation->base == address) {
            return remaining;
        }
        if (best == 0 || remaining < best) {
            best = remaining;
        }
    }
    return best;
}

void vyx_pointer_invalidate_registered_span(void* base,
                                            std::uint64_t bytes) noexcept {
    if (!base || bytes == 0) {
        return;
    }
    auto& registry = vyx_pointer_region_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (auto* allocation = registry.allocations;
         allocation;
         allocation = allocation->next) {
        if (allocation->alive
            && vyx_pointer_address_in_span(allocation->base, base, bytes)) {
            allocation->alive = false;
        }
    }
    for (auto* region = registry.head; region; region = region->next) {
        if (region->alive.load(std::memory_order_acquire) != 0
            && vyx_pointer_address_in_span(region->base, base, bytes)) {
            region->alive.store(0, std::memory_order_release);
            region->base = nullptr;
            region->bytes = 0;
        }
    }
}

void* vyx_pointer_checked_address_impl(void* address,
                                       void* metadata,
                                       std::int64_t index,
                                       std::int64_t element_size,
                                       std::int32_t pointer_depth) noexcept {
    auto* region = vyx_pointer_region_from_metadata(metadata);
    if (!address || !region) {
        vyx_pointer_fail("vyx: empty pointer handle dereference");
    }
    if (region->alive.load(std::memory_order_acquire) == 0) {
        vyx_pointer_fail("vyx: dangling pointer handle dereference");
    }
    if (index < 0 || element_size <= 0) {
        vyx_pointer_fail("vyx: pointer index out of bounds");
    }

    auto access_size = static_cast<std::uint64_t>(element_size);
    auto stride = access_size;
    std::uint64_t extent = 0;
    bool shaped = false;
    if (pointer_depth > 0 && region->rank >= pointer_depth
        && region->dimensions) {
        const auto level = region->rank - pointer_depth;
        if (level >= 0 && level < region->rank) {
            const auto& dimension = region->dimensions[level];
            if (dimension.extent > 0 && dimension.stride > 0) {
                shaped = true;
                extent = dimension.extent;
                stride = dimension.stride;
                access_size = stride;
            }
        }
    }

    const auto element = static_cast<std::uint64_t>(index);
    if (shaped && element >= extent) {
        vyx_pointer_fail("vyx: pointer index out of bounds");
    }
    if (element > std::numeric_limits<std::uint64_t>::max() / stride) {
        vyx_pointer_fail("vyx: pointer index out of bounds");
    }

    const auto base = reinterpret_cast<std::uintptr_t>(region->base);
    const auto current = reinterpret_cast<std::uintptr_t>(address);
    if (current < base || current - base > region->bytes) {
        vyx_pointer_fail("vyx: pointer view lies outside its region");
    }
    const auto offset = static_cast<std::uint64_t>(current - base);
    const auto delta = element * stride;
    if (delta > region->bytes || offset > region->bytes - delta
        || access_size > region->bytes - offset - delta) {
        vyx_pointer_fail("vyx: pointer index out of bounds");
    }
    if (delta > std::numeric_limits<std::uintptr_t>::max() - current) {
        vyx_pointer_fail("vyx: pointer address overflow");
    }
    return reinterpret_cast<void*>(current + static_cast<std::uintptr_t>(delta));
}

} // namespace

extern "C" VYX_POINTER_RT_ABI void* vyx_pointer_region_create(
    void* base,
    std::int64_t bytes,
    std::int32_t owned,
    std::int32_t capability) noexcept {
    if (!base) {
        return nullptr;
    }
    const auto registered_bytes = vyx_pointer_registered_remaining(base);
    if (registered_bytes > 0) {
        bytes = registered_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(registered_bytes);
    }
    if (bytes <= 0) {
        return nullptr;
    }
    if (capability < 0 || capability > 2) {
        vyx_pointer_fail("vyx: invalid pointer capability");
    }
    void* storage = std::malloc(sizeof(VyxPointerRegion));
    if (!storage) {
        return nullptr;
    }
    auto* region = ::new (storage) VyxPointerRegion();
    region->base = base;
    region->bytes = static_cast<std::uint64_t>(bytes);
    region->owned = owned != 0 ? 1 : 0;
    region->alive.store(1, std::memory_order_release);
    auto& registry = vyx_pointer_region_registry();
    {
        std::lock_guard<std::mutex> lock(registry.mutex);
        region->next = registry.head;
        registry.head = region;
    }
    return vyx_pointer_tag_metadata(
        region, static_cast<VyxPointerCapability>(capability));
}

extern "C" VYX_POINTER_RT_ABI void* vyx_pointer_allocation_register(
    void* base,
    std::int64_t bytes) noexcept {
    if (!base || bytes <= 0) {
        return base;
    }
    auto& registry = vyx_pointer_region_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (auto* allocation = registry.allocations;
         allocation;
         allocation = allocation->next) {
        if (allocation->base == base) {
            allocation->bytes = static_cast<std::uint64_t>(bytes);
            allocation->alive = true;
            return base;
        }
    }
    void* storage = std::malloc(sizeof(VyxPointerRegionRegistry::RawAllocation));
    if (!storage) {
        vyx_pointer_fail("vyx: cannot allocate raw allocation metadata");
    }
    auto* allocation = ::new (storage) VyxPointerRegionRegistry::RawAllocation();
    allocation->base = base;
    allocation->bytes = static_cast<std::uint64_t>(bytes);
    allocation->alive = true;
    allocation->next = registry.allocations;
    registry.allocations = allocation;
    return base;
}

extern "C" VYX_POINTER_RT_ABI void vyx_pointer_allocation_unregister(
    void* base) noexcept {
    if (!base) {
        return;
    }
    std::uint64_t bytes = 1;
    {
        auto& registry = vyx_pointer_region_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        for (auto* allocation = registry.allocations;
             allocation;
             allocation = allocation->next) {
            if (allocation->alive && allocation->base == base) {
                bytes = allocation->bytes;
                break;
            }
        }
    }
    vyx_pointer_invalidate_registered_span(base, bytes);
}

extern "C" VYX_POINTER_RT_ABI void vyx_pointer_allocation_unregister_range(
    void* base,
    std::int64_t bytes) noexcept {
    if (!base || bytes <= 0) {
        return;
    }
    vyx_pointer_invalidate_registered_span(base, static_cast<std::uint64_t>(bytes));
}

extern "C" VYX_POINTER_RT_ABI void* vyx_pointer_allocation_realloc(
    void* old_base,
    void* new_base,
    std::int64_t bytes) noexcept {
    if (!new_base) {
        return nullptr;
    }
    if (old_base) {
        vyx_pointer_allocation_unregister(old_base);
    }
    return vyx_pointer_allocation_register(new_base, bytes);
}

extern "C" VYX_POINTER_RT_ABI void* vyx_pointer_region_retag(
    void* metadata,
    std::int32_t capability) noexcept {
    if (!metadata) {
        return nullptr;
    }
    if (capability < 0 || capability > 2) {
        vyx_pointer_fail("vyx: invalid pointer capability");
    }
    return vyx_pointer_tag_metadata(
        vyx_pointer_region_from_metadata(metadata),
        static_cast<VyxPointerCapability>(capability));
}

extern "C" VYX_POINTER_RT_ABI void vyx_pointer_region_set_dimension(
    void* metadata,
    std::int32_t rank,
    std::int32_t level,
    std::int64_t extent,
    std::int64_t stride) noexcept {
    auto* region = vyx_pointer_region_from_metadata(metadata);
    if (!region || rank <= 0 || level < 0 || level >= rank
        || extent <= 0 || stride <= 0) {
        vyx_pointer_fail("vyx: invalid pointer array shape");
    }
    auto& registry = vyx_pointer_region_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    if (region->rank != rank || !region->dimensions) {
        auto* dimensions = static_cast<VyxPointerRegion::Dimension*>(
            std::calloc(static_cast<std::size_t>(rank),
                        sizeof(VyxPointerRegion::Dimension)));
        if (!dimensions) {
            vyx_pointer_fail("vyx: cannot allocate pointer array shape");
        }
        std::free(region->dimensions);
        region->dimensions = dimensions;
        region->rank = rank;
    }
    region->dimensions[level].extent = static_cast<std::uint64_t>(extent);
    region->dimensions[level].stride = static_cast<std::uint64_t>(stride);
}

extern "C" VYX_POINTER_RT_ABI std::int32_t vyx_pointer_handle_alive(
    void* address,
    void* metadata) noexcept {
    auto* region = vyx_pointer_region_from_metadata(metadata);
    return address && region && region->alive.load(std::memory_order_acquire) != 0 ? 1 : 0;
}

extern "C" VYX_POINTER_RT_ABI std::int32_t vyx_pointer_region_rank(
    void* metadata) noexcept {
    auto* region = vyx_pointer_region_from_metadata(metadata);
    if (!region) {
        return 0;
    }
    // Shape setters accept i32 rank; the descriptor stores the ABI's i64 word.
    return static_cast<std::int32_t>(region->rank);
}

extern "C" VYX_POINTER_RT_ABI void* vyx_pointer_checked_address(
    void* address,
    void* metadata,
    std::int64_t index,
    std::int64_t element_size) noexcept {
    return vyx_pointer_checked_address_impl(address,
                                            metadata,
                                            index,
                                            element_size,
                                            0);
}

extern "C" VYX_POINTER_RT_ABI void* vyx_pointer_checked_address_typed(
    void* address,
    void* metadata,
    std::int64_t index,
    std::int64_t element_size,
    std::int32_t pointer_depth) noexcept {
    return vyx_pointer_checked_address_impl(address,
                                            metadata,
                                            index,
                                            element_size,
                                            pointer_depth);
}

extern "C" VYX_POINTER_RT_ABI void* vyx_pointer_region_delete(
    void* metadata) noexcept {
    auto* region = vyx_pointer_region_from_metadata(metadata);
    if (!region) {
        return nullptr;
    }
    const auto capability = vyx_pointer_capability(metadata);
    if (capability == VyxPointerCapability::Shared
        || capability == VyxPointerCapability::Mutable) {
        vyx_pointer_fail("vyx: cannot delete borrowed pointer handle");
    }
    std::int64_t expected = 1;
    if (!region->alive.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return nullptr;
    }
    void* released = region->owned ? region->base : nullptr;
    region->base = nullptr;
    region->bytes = 0;
    return released;
}

#undef VYX_POINTER_RT_ABI
