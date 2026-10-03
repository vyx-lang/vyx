#pragma once

struct Vec2 {
    double x, y;

    double norm1() const noexcept;
};

double Vec2::norm1() const noexcept { return x + y; }

template <typename A, typename B>
struct Pair2 {
    A first;
    B second;

    A get_first() const noexcept { return first; }

    Pair2<B, A> swapped() const noexcept { return Pair2<B, A>{second, first}; }
};

template <typename T>
T twice(T v) { return v + v; }

template <typename T>
T max_of(T a, T b) { return a > b ? a : b; }

template <typename T>
T identity(T v) { return v; }

template <typename T, typename U>
Pair2<U, T> swap(T a, U b) { return Pair2<U, T>{b, a}; }

extern "C" Vec2 vyx_vec2_make(double x, double y) noexcept;
extern "C" double vyx_vec2_x(Vec2 v) noexcept;
extern "C" double vyx_vec2_y(Vec2 v) noexcept;

extern "C" Vec2 vyx_vec2_make(double x, double y) noexcept { return Vec2{x, y}; }

extern "C" double vyx_vec2_x(Vec2 v) noexcept { return v.x; }

extern "C" double vyx_vec2_y(Vec2 v) noexcept { return v.y; }
