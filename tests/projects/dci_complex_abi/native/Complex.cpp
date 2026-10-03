#include "Complex.hpp"

namespace {

int g_destroyed_sink_count = 0;
int g_destroyed_big_count = 0;
int g_destroyed_multi_count = 0;
int g_destroyed_vecbox_count = 0;

} // namespace

namespace abi_complex {

LeftBase::LeftBase() noexcept : l(10) {}
LeftBase::LeftBase(int x) noexcept : l(x + 10) {}
LeftBase::~LeftBase() noexcept = default;

int LeftBase::left_virtual(int x) noexcept {
    return l + x + 100;
}

int LeftBase::left_plain(int x) const noexcept {
    return l + x;
}

RightBase::RightBase() noexcept : r(20) {}
RightBase::RightBase(int x) noexcept : r(x + 20) {}
RightBase::~RightBase() noexcept = default;

int RightBase::right_virtual(int x) noexcept {
    return r + x + 200;
}

int RightBase::right_plain(int x) const noexcept {
    return r + x;
}

Multi::Multi(int x) noexcept : LeftBase(x), RightBase(x), own(x + 30) {}
Multi::~Multi() noexcept {
    ++g_destroyed_multi_count;
}

int Multi::left_virtual(int x) noexcept {
    return l + own + x + 1000;
}

int Multi::right_virtual(int x) noexcept {
    return r + own + x + 2000;
}

int Multi::own_value() const noexcept {
    return l + r + own;
}

int Multi::destroyed_count() noexcept {
    return g_destroyed_multi_count;
}

BigState::BigState() noexcept : a(0), b(1), c(2), d(3) {}
BigState::BigState(int seed) noexcept
    : a(seed), b(seed + 1), c(seed + 2), d(seed + 3) {}
BigState::~BigState() noexcept {
    ++g_destroyed_big_count;
}

long long BigState::sum() const noexcept {
    return a + b + c + d;
}

void BigState::mutate(int delta) noexcept {
    a += delta;
    b += delta;
    c += delta;
    d += delta;
}

int BigState::destroyed_count() noexcept {
    return g_destroyed_big_count;
}

VecBox::VecBox() noexcept = default;
VecBox::VecBox(int seed) noexcept {
    values.push_back(seed);
    values.push_back(seed + 1);
}
VecBox::~VecBox() noexcept {
    ++g_destroyed_vecbox_count;
}

int VecBox::size() const noexcept {
    return static_cast<int>(values.size());
}

int VecBox::at(int index) const noexcept {
    return values.at(static_cast<size_t>(index));
}

void VecBox::push(int value) noexcept {
    values.push_back(value);
}

int VecBox::destroyed_count() noexcept {
    return g_destroyed_vecbox_count;
}

AbstractSink::AbstractSink() noexcept = default;
AbstractSink::~AbstractSink() noexcept {
    ++g_destroyed_sink_count;
}

int AbstractSink::destroyed_count() noexcept {
    return g_destroyed_sink_count;
}

NativeDriver::NativeDriver(int bias) noexcept : bias(bias) {}
NativeDriver::~NativeDriver() noexcept = default;

int NativeDriver::dispatch(AbstractSink* sink, int x) const noexcept {
    if (sink == nullptr) {
        return -9999;
    }
    return sink->consume(x) + bias;
}

} // namespace abi_complex

abi_complex::RegisterPair complex_make_register_pair(int first, int second) noexcept {
    return {first, second};
}

abi_complex::RegisterPair complex_offset_register_pair(
    abi_complex::RegisterPair value,
    int delta) noexcept {
    value.first += delta;
    value.second -= delta;
    return value;
}

int complex_register_pair_score(abi_complex::RegisterPair value) noexcept {
    return value.first * 100 + value.second;
}
