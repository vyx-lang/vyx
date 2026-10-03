// Dedicated probe for the bit-field storage-unit limitation of the current DCI
// subset.  A multi-field bit-field whose later members cross the first storage
// byte causes the C++ adapter to emit a per-field storage unit whose
// storage_offset + storage_size exceeds the containing record, which its own
// `validate --strict` then rejects.  The harness generates a contract from this
// header to record that behaviour as an honest limitation; it is deliberately
// kept out of the main fixture so the primary contract validates cleanly.
#pragma once

#include <cstdint>

namespace abi {

struct Flags {
    uint32_t kind : 3;
    uint32_t level : 5;
    uint32_t payload : 24;
};

uint32_t flags_payload(Flags f) noexcept;

}  // namespace abi
