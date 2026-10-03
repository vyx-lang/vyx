#include <cstdint>
#include <cstring>
#include <new>
#include "dci_msvc_failure.hpp"
#include "E:/Dev/C++/VyxLan-selfhost-yolo/tools/dci/bench/fixtures/abi_fixtures.hpp"

struct dci_Translated_dci_tr_x_0Buffer_abi_QEAA_AEBV01_Z {
    std::int8_t tag;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_x_0Buffer_abi_QEAA_AEBV01_Z dci_tr_x_0Buffer_abi_QEAA_AEBV01_Z(abi::Buffer* self, const abi::Buffer& p0) noexcept {
    dci_Translated_dci_tr_x_0Buffer_abi_QEAA_AEBV01_Z out{};
    try {
        ::new (static_cast<void*>(self)) abi::Buffer(p0);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_x_0Buffer_abi_QEAA_AEBV01_Z_destroy(dci_Translated_dci_tr_x_0Buffer_abi_QEAA_AEBV01_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
struct dci_Translated_dci_tr_checked_divide_abi_YAHHH_Z {
    std::int8_t tag;
    int32_t ok;
    DciFailure err;
};
extern "C" dci_Translated_dci_tr_checked_divide_abi_YAHHH_Z dci_tr_checked_divide_abi_YAHHH_Z(int32_t p0, int32_t p1) noexcept {
    dci_Translated_dci_tr_checked_divide_abi_YAHHH_Z out{};
    try {
        out.ok = abi::checked_divide(p0, p1);
        out.tag = 0;
        return out;
    } catch (...) {
        out.tag = 1;
        dci_msvc_capture_current(&out.err);
        return out;
    }
}
extern "C" void dci_tr_dci_tr_checked_divide_abi_YAHHH_Z_destroy(dci_Translated_dci_tr_checked_divide_abi_YAHHH_Z* value) noexcept {
    if (value == nullptr) {
        return;
    }
    dci_failure_destroy(&value->err);
}
