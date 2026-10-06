#include "vyx_codegen.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unordered_map>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif
extern "C" void* vyx_rt_string_arena_checkpoint();
extern "C" void vyx_rt_string_arena_release(void*);
extern "C" void vyx_rt_heap_trim();
extern "C" void vyx_forget_string_len(const char*);
#define VYX_RT_ABI VYX_API

// Explicit lifetimes for long-running compiler clients. Outside a frame these
// helpers are ordinary malloc/free: batch compiler allocation policy is unchanged.
struct VyxServiceFrame {
    std::size_t allocation_mark;
    void* string_mark;
    struct Lines { int64_t len; std::vector<int64_t> starts; };
    std::unordered_map<const char*, Lines> lines;
};
static thread_local std::vector<VyxServiceFrame*> vyx_service_frames;
struct VyxServiceAllocation { void* base; void* text; void (*release)(void*); };
static thread_local std::vector<VyxServiceAllocation> vyx_service_allocations;
static thread_local std::unordered_map<void*, std::size_t> vyx_service_allocation_slots;

// Coalesce already queued edits before running Sema. No source analysis or
// protocol payload is retained by this transport readiness check.
extern "C" VYX_RT_ABI int32_t vyx_lsp_input_ready(int32_t timeout_ms) {
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (GetFileType(input) != FILE_TYPE_PIPE)
        return WaitForSingleObject(input, std::max(timeout_ms, 0)) == WAIT_OBJECT_0;
    const auto deadline = GetTickCount64() + std::max(timeout_ms, 0);
    do {
        DWORD available = 0;
        if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) return 1;
        if (available) return 1;
        if (GetTickCount64() >= deadline) return 0;
        Sleep(1);
    } while (true);
#else
    struct pollfd input { STDIN_FILENO, POLLIN, 0 };
    return poll(&input, 1, std::max(timeout_ms, 0)) > 0;
#endif
}

// URI comparisons dominate large symbol walks. Compare decoded bytes without
// constructing two temporary paths for every candidate. '+' remains literal.
struct VyxLspUriCursor {
    const char* p;
    std::size_t n;
    std::size_t i = 0;
    bool drive = false;
    explicit VyxLspUriCursor(const char* value) : p(value ? value : ""), n(std::strlen(p)) {
        if (n >= 7 && std::memcmp(p, "file://", 7) == 0) i = 7;
        if (i + 3 < n && p[i] == '/' && std::isalpha(static_cast<unsigned char>(p[i+1])) && p[i+2] == ':') ++i;
        const auto save = i;
        const int first = next();
        const int second = next();
        drive = first >= 0 && std::isalpha(static_cast<unsigned char>(first)) && second == ':';
        i = save;
    }
    static int hex(int c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    int next() {
        if (i >= n) return -1;
        int c = static_cast<unsigned char>(p[i++]);
        if (c == '%' && i + 1 < n) {
            int hi = hex(p[i]), lo = hex(p[i+1]);
            if (hi >= 0 && lo >= 0) { c = hi * 16 + lo; i += 2; }
        }
        return c == '\\' ? '/' : c;
    }
};
extern "C" VYX_RT_ABI int32_t vyx_lsp_uri_equal(const char* a, const char* b) {
    if (a == b) return 1;
    VyxLspUriCursor left(a), right(b);
    bool first = true;
    while (true) {
        int x = left.next(), y = right.next();
        if (first && left.drive && right.drive) { x = std::tolower(x); y = std::tolower(y); }
        if (x != y) return 0;
        if (x < 0) return 1;
        first = false;
    }
}

// Scan JSON strings once, without allocating a substring at every byte.
// Only object keys (a quoted string followed by ':') can match. Quoted source
// text and escaped quotes in document contents are skipped as one value.
extern "C" VYX_RT_ABI int64_t vyx_lsp_json_key(const char* text, int64_t len,
    const char* key, int64_t key_len, int64_t start, int64_t end) {
    if (!text || !key || len < 0 || key_len < 0) return -1;
    start = std::max<int64_t>(start, 0);
    if (end < 0 || end > len) end = len;
    for (int64_t i = start; i < end; ++i) {
        if (text[i] != '"') continue;
        const int64_t open = i++;
        const int64_t begin = i;
        bool escaped = false;
        while (i < end && text[i] != '"') {
            if (text[i] == '\\') { escaped = true; ++i; }
            ++i;
        }
        if (i >= end) return -1;
        const int64_t close = i;
        int64_t next = close + 1;
        while (next < end && std::isspace(static_cast<unsigned char>(text[next]))) ++next;
        if (!escaped && next < end && text[next] == ':' && close - begin == key_len
            && std::memcmp(text + begin, key, static_cast<std::size_t>(key_len)) == 0) return open;
    }
    return -1;
}

extern "C" VYX_RT_ABI int64_t vyx_lsp_line_start(const char* text, int64_t len, int32_t line) {
    if (!text || len < 0 || line < 0 || vyx_service_frames.empty()) return -1;
    auto& cache = vyx_service_frames.back()->lines;
    auto found = cache.find(text);
    if (found == cache.end() || found->second.len != len) {
        VyxServiceFrame::Lines lines{len, {0}};
        for (int64_t i = 0; i < len; ++i) if (text[i] == '\n') lines.starts.push_back(i + 1);
        found = cache.insert_or_assign(text, std::move(lines)).first;
    }
    const auto& starts = found->second.starts;
    return static_cast<std::size_t>(line) < starts.size() ? starts[line] : -1;
}

extern "C" VYX_RT_ABI int32_t vyx_lsp_utf16_column(const char* text, int64_t len, int32_t line, int32_t column) {
    const auto start = vyx_lsp_line_start(text, len, line);
    if (start < 0 || column <= 0) return 0;
    const auto end = std::min<int64_t>(len, start + column);
    int32_t units = 0;
    for (auto i = start; i < end && text[i] != '\n' && text[i] != '\r';) {
        const auto c = static_cast<unsigned char>(text[i]);
        const int width = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        i += width;
        units += width == 4 ? 2 : 1;
    }
    return units;
}

extern "C" VYX_RT_ABI void* vyx_rt_service_alloc(uint64_t size) {
    if (size == 0 || size > static_cast<uint64_t>(SIZE_MAX)) return nullptr;
    void* p = std::malloc(static_cast<std::size_t>(size));
    if (p && !vyx_service_frames.empty()) {
        vyx_service_allocation_slots.emplace(p, vyx_service_allocations.size());
        vyx_service_allocations.push_back({p, p, std::free});
    }
    return p;
}

extern "C" VYX_RT_ABI void vyx_rt_service_track(void* p, void (*release)(void*)) {
    if (!p || !release || vyx_service_frames.empty()) return;
    const auto index = vyx_service_allocations.size();
    if (!vyx_service_allocation_slots.emplace(p, index).second) {
        std::fprintf(stderr, "error: allocation registered twice in compiler service\n");
        std::abort();
    }
    vyx_service_allocations.push_back({p, p, release});
}
extern "C" VYX_RT_ABI void vyx_rt_service_untrack(void* p) {
    if (!p) return;
    auto found = vyx_service_allocation_slots.find(p);
    if (found != vyx_service_allocation_slots.end()) {
        vyx_forget_string_len(static_cast<const char*>(vyx_service_allocations[found->second].text));
        vyx_service_allocations[found->second] = {nullptr, nullptr, nullptr};
        vyx_service_allocation_slots.erase(found);
    }
    vyx_forget_string_len(static_cast<const char*>(p));
}
extern "C" VYX_RT_ABI void vyx_rt_service_free(void* p) {
    if (!p) return;
    vyx_rt_service_untrack(p);
    std::free(p);
}

extern "C" VYX_RT_ABI void* vyx_rt_service_begin() {
    auto* frame = new VyxServiceFrame;
    frame->allocation_mark = vyx_service_allocations.size();
    frame->string_mark = vyx_rt_string_arena_checkpoint();
    if (!frame->string_mark) {
        delete frame;
        std::fprintf(stderr, "error: cannot allocate compiler service frame\n");
        std::abort();
    }
    vyx_service_frames.push_back(frame);
    return frame;
}

extern "C" VYX_RT_ABI int32_t vyx_rt_service_active() {
    return !vyx_service_frames.empty();
}
extern "C" VYX_RT_ABI void vyx_rt_service_string_alias(void* base, void* text) {
    auto found = vyx_service_allocation_slots.find(base);
    if (found != vyx_service_allocation_slots.end())
        vyx_service_allocations[found->second].text = text;
}

extern "C" VYX_RT_ABI void vyx_rt_service_end(void* mark) {
    auto* frame = static_cast<VyxServiceFrame*>(mark);
    if (!frame || vyx_service_frames.empty() || vyx_service_frames.back() != frame) {
        std::fprintf(stderr, "error: compiler service frames must close in reverse order\n");
        std::abort();
    }
    for (std::size_t i = frame->allocation_mark; i < vyx_service_allocations.size(); ++i) {
        void* p = vyx_service_allocations[i].base;
        if (!p) continue;
        vyx_service_allocation_slots.erase(p);
        vyx_forget_string_len(static_cast<const char*>(vyx_service_allocations[i].text));
        vyx_service_allocations[i].release(p);
    }
    vyx_service_allocations.resize(frame->allocation_mark);
    vyx_rt_string_arena_release(frame->string_mark);
    vyx_service_frames.pop_back();
    delete frame;
    if (vyx_service_frames.empty()) {
        // Release high-water bookkeeping as well as source storage.
        std::vector<VyxServiceAllocation>().swap(vyx_service_allocations);
        std::unordered_map<void*, std::size_t>().swap(vyx_service_allocation_slots);
        vyx_rt_heap_trim();
    }
}
