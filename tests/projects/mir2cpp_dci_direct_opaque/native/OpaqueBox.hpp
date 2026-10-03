#pragma once

#include <cstdint>

#if defined(_WIN32)
#define DCI_OPAQUE_EXPORT __declspec(dllexport)
#else
#define DCI_OPAQUE_EXPORT __attribute__((visibility("default")))
#endif

namespace dci_opaque {

class DCI_OPAQUE_EXPORT OpaqueBox final {
public:
    explicit OpaqueBox(std::int32_t seed) noexcept;
    OpaqueBox(const OpaqueBox&) = delete;
    OpaqueBox& operator=(const OpaqueBox&) = delete;
    OpaqueBox(OpaqueBox&&) = delete;
    OpaqueBox& operator=(OpaqueBox&&) = delete;
    ~OpaqueBox() noexcept;

    bool valid() const noexcept;
    std::int32_t value() const noexcept;
    std::int32_t add(std::int32_t delta) noexcept;

private:
    OpaqueBox* self_;
    std::int32_t value_;
    std::uint32_t guard_;
};

} // namespace dci_opaque

DCI_OPAQUE_EXPORT std::int32_t dci_opaque_free_mix(std::int32_t lhs, std::int32_t rhs) noexcept;
DCI_OPAQUE_EXPORT std::int32_t dci_opaque_live_count() noexcept;
DCI_OPAQUE_EXPORT std::int32_t dci_opaque_destroyed_count() noexcept;
DCI_OPAQUE_EXPORT std::int32_t dci_opaque_invalid_destroy_count() noexcept;
