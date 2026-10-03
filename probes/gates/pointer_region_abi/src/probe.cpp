#include <cstddef>
#include <cstring>
#include <type_traits>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <crtdbg.h>
#endif

#define VYX_POINTER_RT_ABI
#include "../../../../vyx_codegen/src/vyx_pointer_handle_rt.inc"

static_assert(sizeof(void*) == 8, "This probe checks the 64-bit pointer ABI.");
static_assert(std::is_standard_layout<VyxPointerRegion>::value);
static_assert(sizeof(VyxPointerRegion) == 56);
static_assert(offsetof(VyxPointerRegion, base) == 0);
static_assert(offsetof(VyxPointerRegion, bytes) == 8);
static_assert(offsetof(VyxPointerRegion, owned) == 16);
static_assert(offsetof(VyxPointerRegion, alive) == 24);
static_assert(offsetof(VyxPointerRegion, rank) == 32);
static_assert(offsetof(VyxPointerRegion, dimensions) == 40);
static_assert(offsetof(VyxPointerRegion, next) == 48);
static_assert(sizeof(VyxPointerRegion::Dimension) == 16);
static_assert(sizeof(std::atomic<std::int64_t>) == 8);

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "pointer_region_abi: %s\n", message);
        std::exit(1);
    }
}

unsigned char* raw_region(void* metadata) {
    return reinterpret_cast<unsigned char*>(
        reinterpret_cast<std::uintptr_t>(metadata) & ~std::uintptr_t{3});
}

// These numeric offsets intentionally do not use the native field names or the
// shared ABI header constants: a producer/consumer offset mismatch must fail.
template<class T> T inline_load(void* metadata, std::size_t offset) {
    T value{};
    std::memcpy(&value, raw_region(metadata) + offset, sizeof(value));
    return value;
}

std::int64_t inline_alive(void* metadata) {
    auto* alive = reinterpret_cast<std::atomic<std::int64_t>*>(raw_region(metadata) + 24);
    return alive->load(std::memory_order_acquire);
}

std::uint64_t dimension_load(void* dimensions, std::size_t level, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, static_cast<unsigned char*>(dimensions) + level * 16 + offset, 8);
    return value;
}

void* inline_checked_address(void* address, void* metadata, std::uint64_t index,
                             std::uint64_t element_size, std::int64_t depth) {
    require(inline_alive(metadata) == 1, "inline alive@24 is not 1");
    auto base = reinterpret_cast<std::uintptr_t>(inline_load<void*>(metadata, 0));
    auto current = reinterpret_cast<std::uintptr_t>(address);
    auto bytes = inline_load<std::uint64_t>(metadata, 8);
    auto rank = inline_load<std::int64_t>(metadata, 32);
    void* dimensions = inline_load<void*>(metadata, 40);
    std::uint64_t stride = element_size;
    if (depth > 0 && rank >= depth && dimensions) {
        const auto level = static_cast<std::size_t>(rank - depth);
        const auto extent = dimension_load(dimensions, level, 0);
        const auto shape_stride = dimension_load(dimensions, level, 8);
        if (extent > 0 && shape_stride > 0) {
            require(index < extent, "inline shape index outside extent");
            stride = shape_stride;
        }
    }
    require(stride > 0 && index <= std::numeric_limits<std::uint64_t>::max() / stride,
            "inline stride overflow");
    const auto delta = index * stride;
    require(current >= base && current - base <= bytes, "inline view outside region");
    const auto offset = static_cast<std::uint64_t>(current - base);
    require(delta <= bytes && offset <= bytes - delta && stride <= bytes - delta - offset,
            "inline bounds check failed");
    return reinterpret_cast<void*>(current + delta);
}

void require_dead(void* metadata) {
    require(inline_alive(metadata) == 0, "invalidated alive@24 must be 0");
    require(inline_load<void*>(metadata, 0) == nullptr, "invalidated base@0 must be null");
    require(inline_load<std::uint64_t>(metadata, 8) == 0, "invalidated bytes@8 must be 0");
}

void unshaped_regions() {
    std::int64_t first[] = {11, 12, 13, 14};
    std::int64_t second[] = {21, 22, 23, 24};
    std::int64_t third[] = {31, 32, 33, 34};
    void* a = vyx_pointer_region_create(first, sizeof(first), 0, 0);
    void* b = vyx_pointer_region_create(second, sizeof(second), 0, 1);
    void* c = vyx_pointer_region_create(third, sizeof(third), 0, 2);
    require(a && b && c, "could not create three unshaped regions");
    require(inline_load<void*>(b, 48) == raw_region(a), "second region must link to first at next@48");
    require(inline_load<void*>(c, 48) == raw_region(b), "third region must link to second at next@48");
    void* handles[] = {a, b, c};
    std::int64_t* addresses[] = {first, second, third};
    for (int i = 0; i < 3; ++i) {
        require(inline_load<void*>(handles[i], 0) == addresses[i], "base@0 mismatch");
        require(inline_load<std::uint64_t>(handles[i], 8) == sizeof(first), "bytes@8 mismatch");
        require(inline_load<std::int64_t>(handles[i], 16) == 0, "owned@16 mismatch");
        require(inline_alive(handles[i]) == 1, "alive@24 mismatch");
        require(inline_load<std::int64_t>(handles[i], 32) == 0, "unshaped rank@32 must be zero");
        require(inline_load<void*>(handles[i], 40) == nullptr, "unshaped dimensions@40 must be null");
        require(vyx_pointer_region_rank(handles[i]) == 0, "native unshaped rank mismatch");
        void* native = vyx_pointer_checked_address_typed(addresses[i], handles[i], 2, 8, 1);
        void* inlined = inline_checked_address(addresses[i], handles[i], 2, 8, 1);
        require(native == addresses[i] + 2 && native == inlined, "unshaped checked address mismatch");
        require(*static_cast<std::int64_t*>(native) == addresses[i][2], "unshaped checked read mismatch");
        require(vyx_pointer_region_delete(vyx_pointer_region_retag(handles[i], 0)) == nullptr,
                "unowned region must not return storage");
        require_dead(handles[i]);
    }
}

void* create_matrix(std::int64_t (&matrix)[2][3]) {
    void* metadata = vyx_pointer_region_create(matrix, sizeof(matrix), 0, 0);
    require(metadata != nullptr, "matrix region creation failed");
    vyx_pointer_region_set_dimension(metadata, 2, 0, 2, 3 * sizeof(std::int64_t));
    vyx_pointer_region_set_dimension(metadata, 2, 1, 3, sizeof(std::int64_t));
    return metadata;
}

void shaped_region() {
    std::int64_t matrix[2][3] = {{10, 11, 12}, {20, 21, 22}};
    void* metadata = create_matrix(matrix);
    require(inline_load<std::int64_t>(metadata, 32) == 2, "rank2 inline rank@32 mismatch");
    require(vyx_pointer_region_rank(metadata) == 2, "rank2 native rank mismatch");
    void* dimensions = inline_load<void*>(metadata, 40);
    require(dimensions != nullptr, "rank2 dimensions@40 is null");
    require(dimension_load(dimensions, 0, 0) == 2 && dimension_load(dimensions, 0, 8) == 24,
            "rank2 outer shape mismatch");
    require(dimension_load(dimensions, 1, 0) == 3 && dimension_load(dimensions, 1, 8) == 8,
            "rank2 inner shape mismatch");
    void* row = vyx_pointer_checked_address_typed(matrix, metadata, 1, 8, 2);
    require(row == matrix[1] && row == inline_checked_address(matrix, metadata, 1, 8, 2),
            "rank2 row address mismatch");
    void* element = vyx_pointer_checked_address_typed(row, metadata, 2, 8, 1);
    require(element == &matrix[1][2] && element == inline_checked_address(row, metadata, 2, 8, 1),
            "rank2 element address mismatch");
    require(*static_cast<std::int64_t*>(element) == 22, "rank2 checked read mismatch");
    vyx_pointer_region_delete(metadata);
    require_dead(metadata);
}

void owned_delete_once() {
    void* storage = std::malloc(64);
    require(storage != nullptr, "owned allocation failed");
    void* metadata = vyx_pointer_region_create(storage, 64, 1, 0);
    require(metadata != nullptr && inline_load<std::int64_t>(metadata, 16) == 1, "owned@16 must be 1");
    require(vyx_pointer_region_delete(metadata) == storage, "owned delete must return original allocation");
    require_dead(metadata);
    require(vyx_pointer_region_delete(metadata) == nullptr, "owned delete must return allocation only once");
    std::free(storage);
}

void range_unregister() {
    auto* storage = static_cast<unsigned char*>(std::malloc(128));
    require(storage != nullptr, "range allocation failed");
    std::int64_t unrelated[] = {41, 42};
    vyx_pointer_allocation_register(storage, 128);
    vyx_pointer_allocation_register(storage + 32, 32);
    void* original = vyx_pointer_region_create(storage, 1, 0, 0);
    void* alias_a = vyx_pointer_region_create(storage + 16, 1, 0, 1);
    void* alias_b = vyx_pointer_region_create(storage + 32, 1, 0, 2);
    void* outside = vyx_pointer_region_create(unrelated, sizeof(unrelated), 0, 0);
    require(original && alias_a && alias_b && outside, "range region creation failed");
    require(inline_load<std::uint64_t>(original, 8) == 128, "registered allocation extent not propagated");
    require(inline_load<std::uint64_t>(alias_a, 8) == 112, "interior alias extent mismatch");
    require(inline_load<std::uint64_t>(alias_b, 8) == 32, "registered interior allocation extent mismatch");
    vyx_pointer_allocation_unregister_range(storage, 128);
    require_dead(original);
    require_dead(alias_a);
    require_dead(alias_b);
    require(vyx_pointer_handle_alive(storage, original) == 0, "native original handle remains alive");
    require(vyx_pointer_handle_alive(storage + 16, alias_a) == 0, "native alias remains alive");
    require(vyx_pointer_registered_remaining(storage) == 0 && vyx_pointer_registered_remaining(storage + 32) == 0,
            "registered original/interior allocations remain live");
    require(inline_alive(outside) == 1, "range invalidation killed an unrelated region");
    require(*static_cast<std::int64_t*>(vyx_pointer_checked_address(unrelated, outside, 1, 8)) == 42,
            "unrelated checked read failed after range invalidation");
    vyx_pointer_region_delete(outside);
    std::free(storage);
}
} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    const char* mode = argc > 1 ? argv[1] : "positive";
    if (std::strcmp(mode, "positive") == 0) {
        unshaped_regions();
        shaped_region();
        owned_delete_once();
        range_unregister();
        std::puts("pointer_region_abi: positive OK");
        return 0;
    }
    std::int64_t values[] = {1, 2, 3, 4};
    void* metadata = vyx_pointer_region_create(values, sizeof(values), 0, 0);
    require(metadata != nullptr, "negative-test region creation failed");
    if (std::strcmp(mode, "borrow_shared_delete") == 0) {
        vyx_pointer_region_delete(vyx_pointer_region_retag(metadata, 1));
    } else if (std::strcmp(mode, "borrow_mutable_delete") == 0) {
        vyx_pointer_region_delete(vyx_pointer_region_retag(metadata, 2));
    } else if (std::strcmp(mode, "dangling") == 0) {
        vyx_pointer_region_delete(metadata);
        vyx_pointer_checked_address(values, metadata, 0, 8);
    } else if (std::strcmp(mode, "range_dangling") == 0) {
        vyx_pointer_allocation_unregister_range(values, sizeof(values));
        vyx_pointer_checked_address(values, metadata, 0, 8);
    } else if (std::strcmp(mode, "shape_oob") == 0) {
        std::int64_t matrix[2][3]{};
        void* shaped = create_matrix(matrix);
        // Index 3 is still inside the full 48-byte allocation. Only the inner
        // dimension's extent check rejects this access, not flat byte bounds.
        vyx_pointer_checked_address_typed(matrix, shaped, 3, 8, 1);
    } else if (std::strcmp(mode, "bounds_oob") == 0) {
        vyx_pointer_checked_address(values, metadata, 4, 8);
    } else {
        std::fprintf(stderr, "unknown probe mode: %s\n", mode);
        return 2;
    }
    std::fprintf(stderr, "pointer_region_abi: expected runtime rejection did not happen: %s\n", mode);
    return 1;
}
