#pragma once

#include <vector>

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

namespace abi_complex {

using IntVector = std::vector<int>;

struct EXPORT RegisterPair {
    int first;
    int second;
};

class EXPORT LeftBase {
public:
    int l;
    LeftBase() noexcept;
    explicit LeftBase(int x) noexcept;
    virtual ~LeftBase() noexcept;
    virtual int left_virtual(int x) noexcept;
    int left_plain(int x) const noexcept;
};

class EXPORT RightBase {
public:
    int r;
    RightBase() noexcept;
    explicit RightBase(int x) noexcept;
    virtual ~RightBase() noexcept;
    virtual int right_virtual(int x) noexcept;
    int right_plain(int x) const noexcept;
};

class EXPORT Multi final : public LeftBase, public RightBase {
public:
    int own;
    explicit Multi(int x) noexcept;
    ~Multi() noexcept override;
    int left_virtual(int x) noexcept override;
    int right_virtual(int x) noexcept override;
    int own_value() const noexcept;
    static int destroyed_count() noexcept;
};

class EXPORT BigState {
public:
    long long a;
    long long b;
    long long c;
    long long d;
    BigState() noexcept;
    explicit BigState(int seed) noexcept;
    ~BigState() noexcept;
    long long sum() const noexcept;
    void mutate(int delta) noexcept;
    static int destroyed_count() noexcept;
};

class EXPORT VecBox {
public:
    IntVector values;
    VecBox() noexcept;
    explicit VecBox(int seed) noexcept;
    ~VecBox() noexcept;
    int size() const noexcept;
    int at(int index) const noexcept;
    void push(int value) noexcept;
    static int destroyed_count() noexcept;
};

class EXPORT AbstractSink {
public:
    AbstractSink() noexcept;
    virtual ~AbstractSink() noexcept;
    virtual int consume(int x) noexcept = 0;
    static int destroyed_count() noexcept;
};

class EXPORT NativeDriver {
public:
    int bias;
    explicit NativeDriver(int bias) noexcept;
    ~NativeDriver() noexcept;
    int dispatch(AbstractSink* sink, int x) const noexcept;
};

} // namespace abi_complex

EXPORT abi_complex::RegisterPair complex_make_register_pair(int first, int second) noexcept;
EXPORT abi_complex::RegisterPair complex_offset_register_pair(abi_complex::RegisterPair value, int delta) noexcept;
EXPORT int complex_register_pair_score(abi_complex::RegisterPair value) noexcept;

/* dci-ownership
{
  "abi_complex::NativeDriver::dispatch(AbstractSink*,int) const": {
    "parameters": { "0": "borrow_mut" }
  }
}
dci-ownership-end */
