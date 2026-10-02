// Ground-truth ABI probe.  Compiled and run with the SAME compiler the DCI
// adapter uses so its sizeof/alignof/offsetof numbers are the ABI authority the
// three binding paths are checked against.  Emits one JSON object on stdout.
#include "abi_fixtures.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

using namespace abi;

int main() {
    std::printf("{\n");
    std::printf("  \"types\": {\n");
    std::printf("    \"Point\": {\"size\": %zu, \"align\": %zu, \"offset_x\": %zu, \"offset_y\": %zu},\n",
                sizeof(Point), alignof(Point), offsetof(Point, x), offsetof(Point, y));
    std::printf("    \"Rect\": {\"size\": %zu, \"align\": %zu, \"offset_origin\": %zu, \"offset_w\": %zu, \"offset_h\": %zu},\n",
                sizeof(Rect), alignof(Rect), offsetof(Rect, origin), offsetof(Rect, w), offsetof(Rect, h));
    std::printf("    \"Vec2\": {\"size\": %zu, \"align\": %zu},\n", sizeof(Vec2), alignof(Vec2));
    std::printf("    \"Span\": {\"size\": %zu, \"align\": %zu, \"offset_data\": %zu, \"offset_len\": %zu},\n",
                sizeof(Span), alignof(Span), offsetof(Span, data), offsetof(Span, len));
    std::printf("    \"Quad\": {\"size\": %zu, \"align\": %zu},\n", sizeof(Quad), alignof(Quad));
    std::printf("    \"PackedHeader\": {\"size\": %zu, \"align\": %zu, \"offset_tag\": %zu, \"offset_length\": %zu},\n",
                sizeof(PackedHeader), alignof(PackedHeader), offsetof(PackedHeader, tag), offsetof(PackedHeader, length));
    std::printf("    \"Buffer\": {\"size\": %zu, \"align\": %zu, \"trivially_copyable\": %d, \"trivially_destructible\": %d},\n",
                sizeof(Buffer), alignof(Buffer),
                std::is_trivially_copyable<Buffer>::value ? 1 : 0,
                std::is_trivially_destructible<Buffer>::value ? 1 : 0);
    std::printf("    \"Circle\": {\"size\": %zu, \"align\": %zu, \"polymorphic\": %d},\n",
                sizeof(Circle), alignof(Circle), std::is_polymorphic<Circle>::value ? 1 : 0);
    std::printf("    \"Color\": {\"size\": %zu, \"align\": %zu},\n", sizeof(Color), alignof(Color));
    std::printf("    \"Status\": {\"size\": %zu, \"align\": %zu}\n", sizeof(Status), alignof(Status));
    std::printf("  },\n");
    // Known-answer results so a caller can verify each binding path runs the
    // real C++ code and observes the correct value.
    Point p{3, 4};
    Rect r{{1, 2}, 5, 6};
    Vec2 a{1.5, 2.0};
    Vec2 b{3.0, 4.0};
    Quad q{1, 2, 3, 4};
    const uint8_t bytes[4] = {1, 2, 3, 4};
    Span s{bytes, 4};
    std::printf("  \"results\": {\n");
    std::printf("    \"point_sum\": %d,\n", point_sum(p));
    std::printf("    \"rect_area\": %d,\n", rect_area(r));
    std::printf("    \"vec2_dot\": %.1f,\n", vec2_dot(a, b));
    std::printf("    \"span_checksum\": %llu,\n", (unsigned long long)span_checksum(s));
    std::printf("    \"quad_sum\": %lld,\n", (long long)quad_sum(q));
    std::printf("    \"color_next\": %d,\n", (int)color_next(Color::Green));
    std::printf("    \"status_step\": %d\n", (int)status_step(Ok));
    std::printf("  }\n");
    std::printf("}\n");
    return 0;
}
