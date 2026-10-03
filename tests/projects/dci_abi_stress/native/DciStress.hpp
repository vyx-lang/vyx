#pragma once

#include <cstdint>
#include <vector>

#if defined(_WIN32)
#define DCI_EXPORT __declspec(dllexport)
#else
#define DCI_EXPORT __attribute__((visibility("default")))
#endif

namespace dci_stress {

enum class Color : int {
    Red = 1,
    Green = 2,
    Blue = 3,
};

class DCI_EXPORT RefBox {
public:
    int value;
    RefBox() noexcept;
    explicit RefBox(int x) noexcept;
    ~RefBox() noexcept;
    int read_ref(const int& x) const noexcept;
    void add_ref(int& x, int delta) const noexcept;
    int self_ref_sum(const RefBox& other) const noexcept;
    RefBox& assign_value(int x) noexcept;
};

template <typename T>
class TemplateBox {
public:
    T value;
    TemplateBox() noexcept;
    explicit TemplateBox(T x) noexcept;
    ~TemplateBox() noexcept;
    T get() const noexcept;
    void set(T x) noexcept;
    TemplateBox& add_assign(T x) noexcept;
};

using IntTemplateBox = TemplateBox<int>;
using IntTemplateBoxAlias = IntTemplateBox;

struct IntTag {};

template <typename T, typename Tag>
class TaggedBox {
public:
    T value;
    int tag;
    TaggedBox() noexcept;
    TaggedBox(T value, int tag) noexcept;
    ~TaggedBox() noexcept;
    T get() const noexcept;
    int tag_value() const noexcept;
    T mix(T extra) const noexcept;
    TaggedBox& set_pair(T value, int tag) noexcept;
};

using TaggedIntBox = TaggedBox<int, IntTag>;
using TaggedIntBoxAlias = TaggedIntBox;

template <typename T>
class TemplatePolyBase {
public:
    T base;
    TemplatePolyBase() noexcept;
    explicit TemplatePolyBase(T base) noexcept;
    virtual ~TemplatePolyBase() noexcept;
    virtual T calc(T x) noexcept;
};

template <typename T>
class TemplatePolyFinal final : public TemplatePolyBase<T> {
public:
    T extra;
    TemplatePolyFinal() noexcept;
    explicit TemplatePolyFinal(T base) noexcept;
    ~TemplatePolyFinal() noexcept override;
    T calc(T x) noexcept override final;
    T total() const noexcept;
};

using IntTemplatePolyBase = TemplatePolyBase<int>;
using IntTemplatePoly = TemplatePolyFinal<int>;
using IntVector = std::vector<int>;

class DCI_EXPORT OpBox {
public:
    int values[4];
    OpBox() noexcept;
    explicit OpBox(int seed) noexcept;
    ~OpBox() noexcept;
    int operator[](int index) const noexcept;
    int operator+(int rhs) const noexcept;
    int operator==(const OpBox& rhs) const noexcept;
    int sum() const noexcept;
    void set(int index, int value) noexcept;
};

struct DCI_EXPORT BitPack {
    unsigned a : 3;
    unsigned b : 5;
    unsigned c : 6;
    int tail;
    BitPack() noexcept;
    explicit BitPack(int seed) noexcept;
    int sum() const noexcept;
    void set(unsigned na, unsigned nb, unsigned nc, int ntail) noexcept;
};

class DCI_EXPORT VRoot {
public:
    int root;
    VRoot() noexcept;
    explicit VRoot(int x) noexcept;
    virtual ~VRoot() noexcept;
    virtual int value(int x) noexcept;
};

class DCI_EXPORT VLeft : virtual public VRoot {
public:
    int left;
    VLeft() noexcept;
    explicit VLeft(int x) noexcept;
    ~VLeft() noexcept override;
    int left_only() const noexcept;
};

class DCI_EXPORT VRight : virtual public VRoot {
public:
    int right;
    VRight() noexcept;
    explicit VRight(int x) noexcept;
    ~VRight() noexcept override;
    int right_only() const noexcept;
};

class DCI_EXPORT VDiamond final : public VLeft, public VRight {
public:
    int own;
    VDiamond() noexcept;
    explicit VDiamond(int x) noexcept;
    ~VDiamond() noexcept override;
    int value(int x) noexcept override;
    virtual int total() const noexcept;
};

DCI_EXPORT int color_value(Color c) noexcept;
DCI_EXPORT Color make_color(int v) noexcept;
DCI_EXPORT int read_const_i32_ref(const int& x) noexcept;
DCI_EXPORT void add_i32_ref(int& x, int delta) noexcept;
DCI_EXPORT int refbox_ref_sum(const RefBox& box, const RefBox& other) noexcept;
DCI_EXPORT int template_box_sum(const IntTemplateBox& box, int extra) noexcept;
DCI_EXPORT int template_box_mutate(IntTemplateBox* box, int delta) noexcept;
DCI_EXPORT int template_box_alias_sum(const IntTemplateBoxAlias& box, int extra) noexcept;
DCI_EXPORT int tagged_box_sum(const TaggedIntBox& box, int extra) noexcept;
DCI_EXPORT int tagged_box_alias_sum(const TaggedIntBoxAlias& box, int extra) noexcept;
DCI_EXPORT int poly_calc_base(IntTemplatePolyBase* box, int x) noexcept;
DCI_EXPORT int poly_calc_derived(IntTemplatePoly* box, int x) noexcept;
DCI_EXPORT IntVector* int_vector_new() noexcept;
DCI_EXPORT void int_vector_delete(IntVector* v) noexcept;
DCI_EXPORT void int_vector_push(IntVector* v, int value) noexcept;
DCI_EXPORT int int_vector_get(const IntVector* v, int index) noexcept;
DCI_EXPORT int int_vector_size(const IntVector* v) noexcept;
DCI_EXPORT int call_root_value(VRoot* p, int x) noexcept;
DCI_EXPORT int call_left_root(VLeft* p, int x) noexcept;
DCI_EXPORT int call_right_root(VRight* p, int x) noexcept;
DCI_EXPORT int bitpack_sum_ptr(const BitPack* p) noexcept;

} // namespace dci_stress

/* dci-ownership
{
  "dci_stress::RefBox::assign_value(int)": {
    "return": "borrow"
  },
  "dci_stress::IntTemplateBox::add_assign(int)": {
    "return": "borrow"
  },
  "dci_stress::IntTemplateBoxAlias::add_assign(int)": {
    "return": "borrow"
  },
  "dci_stress::TaggedIntBox::set_pair(int,int)": {
    "return": "borrow"
  },
  "dci_stress::TaggedIntBoxAlias::set_pair(int,int)": {
    "return": "borrow"
  },
  "template_box_mutate(IntTemplateBox*,int)": {
    "parameters": { "0": "borrow_mut" }
  },
  "poly_calc_base(IntTemplatePolyBase*,int)": {
    "parameters": { "0": "borrow_mut" }
  },
  "poly_calc_derived(IntTemplatePoly*,int)": {
    "parameters": { "0": "borrow_mut" }
  },
  "int_vector_new()": {
    "return": "owned"
  },
  "int_vector_delete(IntVector*)": {
    "parameters": { "0": "move" }
  },
  "int_vector_push(IntVector*,int)": {
    "parameters": { "0": "borrow_mut" }
  },
  "int_vector_get(const IntVector*,int)": {
    "parameters": { "0": "borrow" }
  },
  "int_vector_size(const IntVector*)": {
    "parameters": { "0": "borrow" }
  },
  "call_root_value(VRoot*,int)": {
    "parameters": { "0": "borrow_mut" }
  },
  "call_left_root(VLeft*,int)": {
    "parameters": { "0": "borrow_mut" }
  },
  "call_right_root(VRight*,int)": {
    "parameters": { "0": "borrow_mut" }
  },
  "bitpack_sum_ptr(const BitPack*)": {
    "parameters": { "0": "borrow" }
  }
}
dci-ownership-end */
