#include "A.hpp"

static int g_live_a_count = 0;
static int g_destroyed_a_count = 0;
static int g_live_poly_count = 0;
static int g_destroyed_poly_count = 0;

A::A() noexcept : data(0) {
    ++g_live_a_count;
}

A::A(int x) noexcept : data(x) {
    ++g_live_a_count;
}

A::~A() noexcept {
    --g_live_a_count;
    ++g_destroyed_a_count;
}

void A::func() noexcept {
    data += 100;
}

int A::add(int x) noexcept {
    return data + x;
}

int A::add(double x) noexcept {
    return data + static_cast<int>(x) + 2000;
}

A* A::self_ptr() noexcept {
    return this;
}

int A::add_ptr(const A* p) const noexcept {
    if (p == nullptr) {
        return -9999;
    }
    return data + p->data + 300;
}

int A::bump(int x) noexcept {
    return x + 10;
}

int A::bump(double x) noexcept {
    return static_cast<int>(x) + 100;
}

int A::read_ptr(const A* p) noexcept {
    if (p == nullptr) {
        return -9999;
    }
    return p->data + 400;
}

int free_add(int x, int y) noexcept {
    return x + y + 20;
}

int free_add(double x, double y) noexcept {
    return static_cast<int>(x) + static_cast<int>(y) + 200;
}

NativeDerived::NativeDerived() noexcept : A(9000) {}
NativeDerived::NativeDerived(int x) noexcept : A(x) {}
NativeDerived::~NativeDerived() noexcept = default;

int NativeDerived::add(int x) noexcept {
    return data + x + 1000;
}

int NativeDerived::add(double x) noexcept {
    return data + static_cast<int>(x) + 3000;
}

LeftPad::LeftPad() noexcept : pad(700) {}
LeftPad::~LeftPad() noexcept = default;

int LeftPad::left() const noexcept {
    return pad;
}

RightDerived::RightDerived() noexcept : LeftPad(), A(4000) {}
RightDerived::RightDerived(int x) noexcept : LeftPad(), A(x) {}
RightDerived::~RightDerived() noexcept = default;

int RightDerived::own() const noexcept {
    return pad + data;
}

SharedBox::SharedBox() noexcept : value(std::make_shared<A>(0)) {}
SharedBox::SharedBox(int x) noexcept : value(std::make_shared<A>(x)) {}
SharedBox::~SharedBox() noexcept = default;

int SharedBox::use_count() const noexcept {
    return static_cast<int>(value.use_count());
}

int SharedBox::add(int x) const noexcept {
    return value->add(x);
}

PolyBase::PolyBase() noexcept {
    ++g_live_poly_count;
}

PolyBase::~PolyBase() noexcept {
    --g_live_poly_count;
    ++g_destroyed_poly_count;
}

int PolyBase::add(int x) noexcept {
    return x;
}

int PolyBase::add(double x) noexcept {
    return static_cast<int>(x) + 10;
}

int PolyBase::mul(int x) noexcept {
    return x * 2;
}

PolyDerived::PolyDerived() noexcept = default;
PolyDerived::~PolyDerived() noexcept = default;

int PolyDerived::add(int x) noexcept {
    return x + 100;
}

int PolyDerived::add(double x) noexcept {
    return static_cast<int>(x) + 500;
}

int PolyDerived::mul(int x) noexcept {
    return x * 20;
}

AbstractOp::AbstractOp() noexcept = default;
AbstractOp::~AbstractOp() noexcept = default;

namespace dci_ns {

NsBox::NsBox() noexcept : value(0) {}
NsBox::NsBox(int x) noexcept : value(x) {}
NsBox::~NsBox() noexcept = default;

int NsBox::add(int x) const noexcept {
    return value + x + 6000;
}

int NsBox::virt(int x) noexcept {
    return value + x + 6100;
}

NsDerived::NsDerived(int x) noexcept : NsBox(x) {}
NsDerived::~NsDerived() noexcept = default;

int NsDerived::virt(int x) noexcept {
    return value + x + 6200;
}

} // namespace dci_ns

A* make_a_ptr(int x) noexcept {
    return new A(x);
}

void destroy_a_ptr(A* p) noexcept {
    delete p;
}

int a_ptr_add(A* p, int x) noexcept {
    if (p == nullptr) {
        return -9999;
    }
    return p->add(x);
}

SharedBox make_shared_box(int x) noexcept {
    return SharedBox(x);
}

int shared_box_add(SharedBox* box, int x) noexcept {
    if (box == nullptr) {
        return -9999;
    }
    return box->add(x);
}

int live_a_count() noexcept {
    return g_live_a_count;
}

int destroyed_a_count() noexcept {
    return g_destroyed_a_count;
}

int live_poly_count() noexcept {
    return g_live_poly_count;
}

int destroyed_poly_count() noexcept {
    return g_destroyed_poly_count;
}

PolyBase* make_poly_derived() noexcept {
    return new PolyDerived();
}

void destroy_poly_base(PolyBase* p) noexcept {
    delete p;
}

int call_poly_add(PolyBase* p, int x) noexcept {
    if (p == nullptr) {
        return -9999;
    }
    return p->add(x);
}

int call_abstract(AbstractOp* p, int x) noexcept {
    if (p == nullptr) {
        return -9999;
    }
    return p->eval(x);
}
