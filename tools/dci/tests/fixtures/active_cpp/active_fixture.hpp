// DCI Active Adapter phase 1 fixture: function templates for the C++ endpoint.
#pragma once

// Closes for arithmetic primitives; rejected for NoAdd (no operator+).
template <typename T>
T tadd(T a, T b) { return a + b; }

// Same producer entity, per-operation admission demo endpoint.
template <typename T>
unsigned tlen(const T &value) { return sizeof(T); }

struct Plain {
    int x;
};

struct NoAdd {};
