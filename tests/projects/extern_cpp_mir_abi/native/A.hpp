#pragma once

#include <memory>

#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

class EXPORT A {
public:
    int data;
    A() noexcept;
    A(int x) noexcept;
    virtual ~A() noexcept;
    void func() noexcept;
    int add(int x) noexcept;
    int add(double x) noexcept;
    A* self_ptr() noexcept;
    int add_ptr(const A* p) const noexcept;
    static int bump(int x) noexcept;
    static int bump(double x) noexcept;
    static int read_ptr(const A* p) noexcept;
};

class EXPORT NativeDerived : public A {
public:
    NativeDerived() noexcept;
    explicit NativeDerived(int x) noexcept;
    ~NativeDerived() noexcept override;
    int add(int x) noexcept;
    int add(double x) noexcept;
};

class EXPORT LeftPad {
public:
    int pad;
    LeftPad() noexcept;
    virtual ~LeftPad() noexcept;
    int left() const noexcept;
};

class EXPORT RightDerived : public LeftPad, public A {
public:
    RightDerived() noexcept;
    explicit RightDerived(int x) noexcept;
    ~RightDerived() noexcept override;
    int own() const noexcept;
};

class EXPORT SharedBox {
public:
    std::shared_ptr<A> value;
    SharedBox() noexcept;
    explicit SharedBox(int x) noexcept;
    ~SharedBox() noexcept;
    int use_count() const noexcept;
    int add(int x) const noexcept;
};

class EXPORT PolyBase {
public:
    PolyBase() noexcept;
    virtual ~PolyBase() noexcept;
    virtual int add(int x) noexcept;
    virtual int add(double x) noexcept;
    virtual int mul(int x) noexcept;
};

class EXPORT PolyDerived final : public PolyBase {
public:
    PolyDerived() noexcept;
    ~PolyDerived() noexcept override;
    int add(int x) noexcept override;
    int add(double x) noexcept override;
    int mul(int x) noexcept override;
};

class EXPORT AbstractOp {
public:
    AbstractOp() noexcept;
    virtual ~AbstractOp() noexcept;
    virtual int eval(int x) noexcept = 0;
};

namespace dci_ns {

class EXPORT NsBox {
public:
    int value;
    NsBox() noexcept;
    explicit NsBox(int x) noexcept;
    virtual ~NsBox() noexcept;
    int add(int x) const noexcept;
    virtual int virt(int x) noexcept;
};

class EXPORT NsDerived : public NsBox {
public:
    explicit NsDerived(int x) noexcept;
    ~NsDerived() noexcept override;
    int virt(int x) noexcept override;
};

} // namespace dci_ns

EXPORT int free_add(int x, int y) noexcept;
EXPORT int free_add(double x, double y) noexcept;
EXPORT A* make_a_ptr(int x) noexcept;
EXPORT void destroy_a_ptr(A* p) noexcept;
EXPORT int a_ptr_add(A* p, int x) noexcept;
EXPORT SharedBox make_shared_box(int x) noexcept;
EXPORT int shared_box_add(SharedBox* box, int x) noexcept;
EXPORT int live_a_count() noexcept;
EXPORT int destroyed_a_count() noexcept;
EXPORT int live_poly_count() noexcept;
EXPORT int destroyed_poly_count() noexcept;
EXPORT PolyBase* make_poly_derived() noexcept;
EXPORT void destroy_poly_base(PolyBase* p) noexcept;
EXPORT int call_poly_add(PolyBase* p, int x) noexcept;
EXPORT int call_abstract(AbstractOp* p, int x) noexcept;

/* dci-ownership
{
  "A::self_ptr()": {
    "return": "borrow"
  },
  "A::add_ptr(const A*) const": {
    "parameters": { "0": "borrow" }
  },
  "A::read_ptr(const A*)": {
    "parameters": { "0": "borrow" }
  },
  "make_a_ptr(int)": {
    "return": "owned"
  },
  "destroy_a_ptr(A*)": {
    "parameters": { "0": "move" }
  },
  "a_ptr_add(A*,int)": {
    "parameters": { "0": "borrow_mut" }
  },
  "shared_box_add(SharedBox*,int)": {
    "parameters": { "0": "borrow" }
  },
  "make_poly_derived()": {
    "return": "owned"
  },
  "destroy_poly_base(PolyBase*)": {
    "parameters": { "0": "move" }
  },
  "call_poly_add(PolyBase*,int)": {
    "parameters": { "0": "borrow_mut" }
  },
  "call_abstract(AbstractOp*,int)": {
    "parameters": { "0": "borrow_mut" }
  }
}
dci-ownership-end */
