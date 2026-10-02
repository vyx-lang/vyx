#include "OpaqueBox.hpp"

namespace {

constexpr std::uint32_t kOpaqueGuard = 0xDCA10A5u;
std::int32_t g_live_count = 0;
std::int32_t g_destroyed_count = 0;
std::int32_t g_invalid_destroy_count = 0;

} // namespace

namespace dci_opaque {

OpaqueBox::OpaqueBox(std::int32_t seed) noexcept
    : self_(this), value_(seed), guard_(kOpaqueGuard) {
    ++g_live_count;
}

OpaqueBox::~OpaqueBox() noexcept {
    if (valid()) {
        --g_live_count;
        ++g_destroyed_count;
    } else {
        ++g_invalid_destroy_count;
    }
    self_ = nullptr;
    value_ = 0;
    guard_ = 0;
}

bool OpaqueBox::valid() const noexcept {
    return self_ == this && guard_ == kOpaqueGuard;
}

std::int32_t OpaqueBox::value() const noexcept {
    return valid() ? value_ : -10001;
}

std::int32_t OpaqueBox::add(std::int32_t delta) noexcept {
    if (!valid()) {
        return -10002;
    }
    value_ += delta;
    return value_;
}

} // namespace dci_opaque

std::int32_t dci_opaque_free_mix(std::int32_t lhs, std::int32_t rhs) noexcept {
    return lhs * 31 + rhs;
}

std::int32_t dci_opaque_live_count() noexcept {
    return g_live_count;
}

std::int32_t dci_opaque_destroyed_count() noexcept {
    return g_destroyed_count;
}

std::int32_t dci_opaque_invalid_destroy_count() noexcept {
    return g_invalid_destroy_count;
}
