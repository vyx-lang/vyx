#include "dci_failure_abi.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <typeinfo>

namespace {
constexpr char expected_message[] = "abi::checked_divide: division by zero";
int destroyed_count = 0;

const char* expected_identity() noexcept {
    // This gate targets the MSVC capture ABI. raw_name is the exact
    // TypeDescriptor spelling copied by dci_msvc_capture_current.
    return typeid(std::runtime_error).raw_name();
}
}

extern "C" std::uint64_t dci_failure_probe_expected_identity_length() noexcept {
    return std::strlen(expected_identity());
}

extern "C" int dci_failure_probe_check_and_destroy(DciFailure* failure) noexcept {
    if (failure == nullptr) { return 40; }
    if (failure->message == nullptr || failure->message_len != sizeof(expected_message) - 1
        || std::memcmp(failure->message, expected_message, sizeof(expected_message) - 1) != 0) {
        return 41;
    }
    const char* identity = expected_identity();
    const auto identity_length = std::strlen(identity);
    if (failure->type_identity == nullptr || failure->type_identity_len != identity_length
        || std::memcmp(failure->type_identity, identity, identity_length) != 0) {
        return 42;
    }
    if (failure->producer_tag != DCI_FAILURE_TAG_MSVC_CXX) { return 43; }
    if (failure->payload == nullptr || failure->payload_size != sizeof(std::runtime_error)
        || failure->payload_align < alignof(std::runtime_error)
        || (failure->payload_align & (failure->payload_align - 1)) != 0
        || reinterpret_cast<std::uintptr_t>(failure->payload) % failure->payload_align != 0) {
        return 44;
    }
    const auto* payload = reinterpret_cast<const std::runtime_error*>(failure->payload);
    if (std::strcmp(payload->what(), expected_message) != 0) { return 45; }

    // The stable record itself belongs to Result. Only the allocated exception
    // contents are destroyed; there is no separately allocated record wrapper.
    dci_failure_destroy(failure);
    const DciFailure empty{};
    if (std::memcmp(failure, &empty, sizeof(empty)) != 0) { return 46; }
    ++destroyed_count;
    return 0;
}

extern "C" int dci_failure_probe_destroyed_count() noexcept {
    return destroyed_count;
}

extern "C" __declspec(noinline) std::uint64_t dci_failure_probe_clobber_stack(std::uint64_t seed) noexcept {
    volatile std::uint64_t values[1024];
    for (std::uint64_t i = 0; i < 1024; ++i) { values[i] = seed + i; }
    return values[0] + values[1023];
}
