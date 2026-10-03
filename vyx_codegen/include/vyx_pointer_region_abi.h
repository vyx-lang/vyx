#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Pointer-region layout v1 is the 56-byte descriptor already consumed by
// runtime/src/vyx_runtime.vyx and llvm_lower.vyx's inline pointer checks.
// It is independent of the exported pointer-handle entry-point ABI version.
// Metadata's two low pointer bits are capability tags, not descriptor fields.
// Changes to this layout require coordinated runtime and codegen migration;
// changing a field to bool or int32_t silently breaks generated machine code.
namespace vyx_pointer_region_abi {

inline constexpr std::uint32_t layout_version = 1;
inline constexpr std::size_t descriptor_size = 56;
inline constexpr std::size_t descriptor_alignment = 8;
inline constexpr std::size_t base_offset = 0;
inline constexpr std::size_t span_offset = 8;
inline constexpr std::size_t owned_offset = 16;
inline constexpr std::size_t alive_offset = 24;
inline constexpr std::size_t rank_offset = 32;
inline constexpr std::size_t dimensions_offset = 40;
inline constexpr std::size_t next_offset = 48;

struct alignas(descriptor_alignment) Region final {
    void* base = nullptr;
    std::uint64_t bytes = 0;
    std::int64_t owned = 0;
    // The canonical runtime can use this word as a live reference count.
    // Native descriptors currently start at one and become zero on deletion
    // or allocation invalidation. Keep the full atomic i64 representation.
    std::atomic<std::int64_t> alive{0};
    std::int64_t rank = 0;
    struct Dimension {
        std::uint64_t extent = 0;
        std::uint64_t stride = 0;
    }* dimensions = nullptr;
    Region* next = nullptr;
};

static_assert(sizeof(void*) == 8, "pointer-region layout v1 requires 64-bit pointers");
static_assert(sizeof(std::int64_t) == 8);
static_assert(sizeof(std::atomic<std::int64_t>) == 8);
static_assert(alignof(std::atomic<std::int64_t>) == 8);
static_assert(std::is_standard_layout_v<Region>);
static_assert(alignof(Region) == descriptor_alignment);
static_assert(sizeof(Region) == descriptor_size);
static_assert(offsetof(Region, base) == base_offset);
static_assert(offsetof(Region, bytes) == span_offset);
static_assert(offsetof(Region, owned) == owned_offset);
static_assert(offsetof(Region, alive) == alive_offset);
static_assert(offsetof(Region, rank) == rank_offset);
static_assert(offsetof(Region, dimensions) == dimensions_offset);
static_assert(offsetof(Region, next) == next_offset);
static_assert(std::is_standard_layout_v<Region::Dimension>);
static_assert(sizeof(Region::Dimension) == 16);
static_assert(alignof(Region::Dimension) == 8);
static_assert(offsetof(Region::Dimension, extent) == 0);
static_assert(offsetof(Region::Dimension, stride) == 8);

} // namespace vyx_pointer_region_abi
