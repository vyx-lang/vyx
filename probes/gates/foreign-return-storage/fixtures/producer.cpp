#include <cstdint>
#include <cstring>

extern "C" const char* gate_index_source(const char* path, std::int64_t* length) {
    static const char source[] = "index-owned source";
    if (!path || std::strcmp(path, "document.vyx") != 0) return nullptr;
    *length = sizeof(source) - 1;
    return source;
}

extern "C" const char* gate_alias(const char* storage, std::int64_t*) {
    return storage;
}
