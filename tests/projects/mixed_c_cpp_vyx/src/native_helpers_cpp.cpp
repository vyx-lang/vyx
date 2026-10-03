#include "mixed_helpers.h"

#ifndef MIXED_CXX_FLAG
#error "expected MIXED_CXX_FLAG to be defined via cxxflags"
#endif

extern "C" int32_t mixed_cpp_helper(int32_t x) {
    // Use a few C++17 niceties to ensure the C++17 path actually compiles.
    constexpr int32_t bonus = MIXED_CXX_FLAG;
    auto add = [bonus](int32_t v) { return v + bonus; };
    return add(x);
}
