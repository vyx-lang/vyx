#include "tools/dci/runtime/dci_itanium_failure.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>

static int g_fail;

static void expect(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", msg);
        g_fail = 1;
    }
}

struct Boom {
    int value;
};

int main() {
    std::fprintf(stderr, "sizeof(DciFailure)=%llu\n",
                 static_cast<unsigned long long>(dci_failure_sizeof()));

    DciFailure f{};
    try {
        throw std::runtime_error("div0");
    } catch (...) {
        expect(dci_itanium_capture_current(&f) != 0, "capture runtime_error");
    }
    expect(f.producer_tag == DCI_FAILURE_TAG_ITANIUM_CXX, "producer_tag");
    expect(f.type_identity != nullptr
               && std::strstr(f.type_identity, "runtime_error") != nullptr,
           "type_identity runtime_error");
    expect(f.message != nullptr && std::strstr(f.message, "div0") != nullptr, "message div0");
    expect(f.payload != nullptr && f.payload_size >= sizeof(std::runtime_error),
           "payload size");

    DciFailure copied{};
    expect(dci_failure_copy(&f, &copied) != 0, "copy runtime_error");
    expect(copied.message != nullptr && std::strstr(copied.message, "div0") != nullptr,
           "copied message");
    dci_failure_destroy(&copied);
    dci_failure_destroy(&f);

    try {
        throw 42;
    } catch (...) {
        expect(dci_itanium_capture_current(&f) != 0, "capture int");
    }
    expect(f.payload_size == 4 && f.payload != nullptr
               && *reinterpret_cast<const int*>(f.payload) == 42,
           "int 42 payload");
    expect(f.message == nullptr, "int has no what()");
    expect(f.type_identity != nullptr
               && (std::strcmp(f.type_identity, "i") == 0
                   || std::strstr(f.type_identity, "int") != nullptr),
           "int type identity");
    dci_failure_destroy(&f);

    try {
        throw Boom{7};
    } catch (...) {
        expect(dci_itanium_capture_current(&f) != 0, "capture Boom");
    }
    expect(f.payload != nullptr && f.payload_size >= sizeof(Boom)
               && reinterpret_cast<const Boom*>(f.payload)->value == 7,
           "Boom payload");
    expect(f.message == nullptr, "Boom has no what()");
    dci_failure_destroy(&f);

    return g_fail;
}
