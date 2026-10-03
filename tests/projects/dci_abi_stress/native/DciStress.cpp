#include "DciStress.hpp"

namespace dci_stress {

RefBox::RefBox() noexcept : value(0) {}
RefBox::RefBox(int x) noexcept : value(x) {}
RefBox::~RefBox() noexcept = default;
int RefBox::read_ref(const int& x) const noexcept { return value + x; }
void RefBox::add_ref(int& x, int delta) const noexcept { x += value + delta; }
int RefBox::self_ref_sum(const RefBox& other) const noexcept { return value + other.value; }
RefBox& RefBox::assign_value(int x) noexcept {
    value = x;
    return *this;
}

template <typename T>
TemplateBox<T>::TemplateBox() noexcept : value(0) {}

template <typename T>
TemplateBox<T>::TemplateBox(T x) noexcept : value(x) {}

template <typename T>
TemplateBox<T>::~TemplateBox() noexcept = default;

template <typename T>
T TemplateBox<T>::get() const noexcept { return value; }

template <typename T>
void TemplateBox<T>::set(T x) noexcept { value = x; }

template <typename T>
TemplateBox<T>& TemplateBox<T>::add_assign(T x) noexcept {
    value += x;
    return *this;
}

template class DCI_EXPORT TemplateBox<int>;

template <typename T, typename Tag>
TaggedBox<T, Tag>::TaggedBox() noexcept : value(0), tag(0) {}

template <typename T, typename Tag>
TaggedBox<T, Tag>::TaggedBox(T value, int tag) noexcept : value(value), tag(tag) {}

template <typename T, typename Tag>
TaggedBox<T, Tag>::~TaggedBox() noexcept = default;

template <typename T, typename Tag>
T TaggedBox<T, Tag>::get() const noexcept { return value; }

template <typename T, typename Tag>
int TaggedBox<T, Tag>::tag_value() const noexcept { return tag; }

template <typename T, typename Tag>
T TaggedBox<T, Tag>::mix(T extra) const noexcept { return value + tag + extra; }

template <typename T, typename Tag>
TaggedBox<T, Tag>& TaggedBox<T, Tag>::set_pair(T value, int tag) noexcept {
    this->value = value;
    this->tag = tag;
    return *this;
}

template class DCI_EXPORT TaggedBox<int, IntTag>;

template <typename T>
TemplatePolyBase<T>::TemplatePolyBase() noexcept : base(0) {}

template <typename T>
TemplatePolyBase<T>::TemplatePolyBase(T base) noexcept : base(base) {}

template <typename T>
TemplatePolyBase<T>::~TemplatePolyBase() noexcept = default;

template <typename T>
T TemplatePolyBase<T>::calc(T x) noexcept { return base + x + 100; }

template <typename T>
TemplatePolyFinal<T>::TemplatePolyFinal() noexcept : TemplatePolyBase<T>(10), extra(20) {}

template <typename T>
TemplatePolyFinal<T>::TemplatePolyFinal(T base) noexcept : TemplatePolyBase<T>(base), extra(base + 5) {}

template <typename T>
TemplatePolyFinal<T>::~TemplatePolyFinal() noexcept = default;

template <typename T>
T TemplatePolyFinal<T>::calc(T x) noexcept { return this->base + extra + x + 1000; }

template <typename T>
T TemplatePolyFinal<T>::total() const noexcept { return this->base + extra; }

template class DCI_EXPORT TemplatePolyBase<int>;
template class DCI_EXPORT TemplatePolyFinal<int>;

OpBox::OpBox() noexcept : values{0, 1, 2, 3} {}
OpBox::OpBox(int seed) noexcept : values{seed, seed + 1, seed + 2, seed + 3} {}
OpBox::~OpBox() noexcept = default;
int OpBox::operator[](int index) const noexcept { return values[index & 3]; }
int OpBox::operator+(int rhs) const noexcept { return sum() + rhs; }
int OpBox::operator==(const OpBox& rhs) const noexcept { return sum() == rhs.sum(); }
int OpBox::sum() const noexcept { return values[0] + values[1] + values[2] + values[3]; }
void OpBox::set(int index, int value) noexcept { values[index & 3] = value; }

BitPack::BitPack() noexcept : a(1), b(2), c(3), tail(4) {}
BitPack::BitPack(int seed) noexcept
    : a(static_cast<unsigned>(seed) & 7U),
      b(static_cast<unsigned>(seed + 1) & 31U),
      c(static_cast<unsigned>(seed + 2) & 63U),
      tail(seed + 3) {}
int BitPack::sum() const noexcept { return static_cast<int>(a + b + c) + tail; }
void BitPack::set(unsigned na, unsigned nb, unsigned nc, int ntail) noexcept {
    a = na;
    b = nb;
    c = nc;
    tail = ntail;
}

VRoot::VRoot() noexcept : root(10) {}
VRoot::VRoot(int x) noexcept : root(x) {}
VRoot::~VRoot() noexcept = default;
int VRoot::value(int x) noexcept { return root + x; }

VLeft::VLeft() noexcept : VRoot(20), left(30) {}
VLeft::VLeft(int x) noexcept : VRoot(x), left(x + 10) {}
VLeft::~VLeft() noexcept = default;
int VLeft::left_only() const noexcept { return left + root; }

VRight::VRight() noexcept : VRoot(40), right(50) {}
VRight::VRight(int x) noexcept : VRoot(x), right(x + 20) {}
VRight::~VRight() noexcept = default;
int VRight::right_only() const noexcept { return right + root; }

VDiamond::VDiamond() noexcept : VRoot(100), VLeft(110), VRight(120), own(130) {}
VDiamond::VDiamond(int x) noexcept : VRoot(x), VLeft(x + 10), VRight(x + 20), own(x + 30) {}
VDiamond::~VDiamond() noexcept = default;
int VDiamond::value(int x) noexcept { return root + left + right + own + x + 1000; }
int VDiamond::total() const noexcept { return root + left + right + own; }

int color_value(Color c) noexcept { return static_cast<int>(c) * 10; }
Color make_color(int v) noexcept {
    if (v == 1) { return Color::Red; }
    if (v == 2) { return Color::Green; }
    return Color::Blue;
}
int read_const_i32_ref(const int& x) noexcept { return x + 100; }
void add_i32_ref(int& x, int delta) noexcept { x += delta + 200; }
int refbox_ref_sum(const RefBox& box, const RefBox& other) noexcept { return box.value + other.value + 300; }
int template_box_sum(const IntTemplateBox& box, int extra) noexcept { return box.get() + extra + 100; }
int template_box_mutate(IntTemplateBox* box, int delta) noexcept {
    box->add_assign(delta);
    return box->get() + 200;
}
int template_box_alias_sum(const IntTemplateBoxAlias& box, int extra) noexcept { return box.get() + extra + 300; }
int tagged_box_sum(const TaggedIntBox& box, int extra) noexcept { return box.mix(extra) + 500; }
int tagged_box_alias_sum(const TaggedIntBoxAlias& box, int extra) noexcept { return box.mix(extra) + 700; }
int poly_calc_base(IntTemplatePolyBase* box, int x) noexcept { return box->calc(x); }
int poly_calc_derived(IntTemplatePoly* box, int x) noexcept { return box->calc(x); }
IntVector* int_vector_new() noexcept { return new IntVector(); }
void int_vector_delete(IntVector* v) noexcept { delete v; }
void int_vector_push(IntVector* v, int value) noexcept { v->push_back(value); }
int int_vector_get(const IntVector* v, int index) noexcept { return (*v)[static_cast<std::size_t>(index)]; }
int int_vector_size(const IntVector* v) noexcept { return static_cast<int>(v->size()); }
int call_root_value(VRoot* p, int x) noexcept { return p->value(x); }
int call_left_root(VLeft* p, int x) noexcept { return p->value(x); }
int call_right_root(VRight* p, int x) noexcept { return p->value(x); }
int bitpack_sum_ptr(const BitPack* p) noexcept { return p->sum(); }

} // namespace dci_stress
