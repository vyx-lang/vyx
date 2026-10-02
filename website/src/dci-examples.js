// Shortened reading examples from dci_opengeneric and dci_multilang.
// Declarations and callers are split into files for display; full build setup
// and contract generation remain in the linked fixtures.
const rust = `#[repr(C)]
#[derive(Clone, Copy)]
pub struct Vec2 {
    pub x: f64,
    pub y: f64,
}

impl Vec2 {
    pub fn norm1(&self) -> f64 {
        self.x + self.y
    }
}

pub fn twice<T: std::ops::Add<Output = T> + Copy>(v: T) -> T {
    v + v
}

pub fn max_of<T: PartialOrd>(a: T, b: T) -> T {
    if a > b { a } else { b }
}

pub fn identity<T>(v: T) -> T {
    v
}

#[repr(C)]
pub struct Pair2<A, B> {
    pub first: A,
    pub second: B,
}

impl<A, B> Pair2<A, B> {
    pub fn get_first(&self) -> A
    where
        A: Copy,
    {
        self.first
    }

    pub fn swapped(&self) -> Pair2<B, A>
    where
        A: Copy,
        B: Copy,
    {
        Pair2 {
            first: self.second,
            second: self.first,
        }
    }
}

pub fn swap<T, U>(a: T, b: U) -> Pair2<U, T> {
    Pair2 { first: b, second: a }
}

pub fn vec2_make(x: f64, y: f64) -> Vec2 {
    Vec2 { x, y }
}

pub fn vec2_x(v: Vec2) -> f64 {
    v.x
}

pub fn vec2_y(v: Vec2) -> f64 {
    v.y
}`

const cpp = `#pragma once

struct Vec2 {
    double x, y;

    double norm1() const noexcept;
};

double Vec2::norm1() const noexcept {
    return x + y;
}

template <typename A, typename B>
struct Pair2 {
    A first;
    B second;

    A get_first() const noexcept {
        return first;
    }

    Pair2<B, A> swapped() const noexcept {
        return Pair2<B, A>{second, first};
    }
};

template <typename T>
T twice(T v) {
    return v + v;
}

template <typename T>
T max_of(T a, T b) {
    return a > b ? a : b;
}

template <typename T>
T identity(T v) {
    return v;
}

template <typename T, typename U>
Pair2<U, T> swap(T a, U b) {
    return Pair2<U, T>{b, a};
}

extern "C" Vec2 vyx_vec2_make(double x, double y) noexcept {
    return Vec2{x, y};
}

extern "C" double vyx_vec2_x(Vec2 v) noexcept {
    return v.x;
}

extern "C" double vyx_vec2_y(Vec2 v) noexcept {
    return v.y;
}`

const bindings = provider => `module native;

@[dci_import("../dci/open_generic${provider === 'cpp' ? '.cpp' : ''}.dcib")]
extern "dci" {
    struct Vec2 {
        public x: f64;
        public y: f64;

        public fn norm1() const -> f64;
    };

    struct Pair2<A, B> {
        public first: A;
        public second: B;

        public fn get_first() const -> A;
        public fn swapped() const -> Pair2<B, A>;
    };

    fn vyx_vec2_make(x: f64, y: f64) -> Vec2;
    fn vyx_vec2_x(value: Vec2) -> f64;
    fn vyx_vec2_y(value: Vec2) -> f64;

    fn twice<T>(value: T) -> T;
    fn max_of<T>(a: T, b: T) -> T;
    fn identity<T>(value: T) -> T;
    fn swap<T, U>(a: T, b: U) -> Pair2<U, T>;
}`

const consumer = provider => `use native;

fn main() -> i32 {
    // The adapter asks the producer compiler for these instances.
    print(twice(21));             // 42
    print(twice(1.5));            // 3.0
    print(max_of(3, 9));          // 9

    // Pass a producer-defined record through a generic.
    let point = vyx_vec2_make(3.0, 4.0);
    let same = identity(point);
    print(same.x);                // 3.0
    print(point.norm1());         // 7.0

    // Call native methods on a generic aggregate.
    let pair = swap(21, 1.5);
    print(pair.get_first());      // 1.5

    let reversed = pair.swapped();
    print(reversed.first);        // 21
    print(reversed.second);       // 1.5
    return 0;
}`

export const dciProviders = [
  { id: 'rust', label: 'Rust', compiler: 'rustc', contract: 'open_generic.dcib', files: [
    { name: 'lib.rs', language: 'Rust', role: { zh: '原始定义', en: 'Producer' }, code: rust },
    { name: 'native.vyx', language: 'Vyx', role: { zh: '契约声明', en: 'Declarations' }, code: bindings('rust') },
    { name: 'main.vyx', language: 'Vyx', role: { zh: '原生调用', en: 'Usage' }, code: consumer('rust') },
  ] },
  { id: 'cpp', label: 'C++', compiler: 'clang++', contract: 'open_generic.cpp.dcib', files: [
    { name: 'lib.hpp', language: 'C++', role: { zh: '原始定义', en: 'Producer' }, code: cpp },
    { name: 'native.vyx', language: 'Vyx', role: { zh: '契约声明', en: 'Declarations' }, code: bindings('cpp') },
    { name: 'main.vyx', language: 'Vyx', role: { zh: '原生调用', en: 'Usage' }, code: consumer('cpp') },
  ] },
]

// dci_multilang consumes the C++ definition from dci_complex_abi and the Rust
// definition from dci_rust_trait. Open generics use a separate contract.
export const dciCases = [
  {
    id: 'inheritance',
    label: { zh: '跨语言继承', en: 'Inheritance' },
    detail: { zh: 'Vyx 类继承 C++ 抽象基类', en: 'Vyx derives from a C++ abstract class' },
    output: 'VyxSink → AbstractSink · consume(1) → 7001',
    source: 'tests/projects/dci_multilang/src/main.vyx',
    providers: [{
      id: 'cpp', label: 'C++', compiler: 'clang++', contract: 'Complex.dcib',
      files: [
        { name: 'Complex.hpp', language: 'C++', role: { zh: '生产端定义', en: 'Producer' }, code: `#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

namespace abi_complex {
class EXPORT AbstractSink {
public:
    AbstractSink() noexcept;
    virtual ~AbstractSink() noexcept;
    virtual int consume(int x) noexcept = 0;
};

class EXPORT NativeDriver {
public:
    int bias;
    explicit NativeDriver(int bias) noexcept;
    int dispatch(AbstractSink* sink, int x) const noexcept;
};
}` },
        { name: 'main.vyx · DCI', language: 'Vyx', role: { zh: '契约声明', en: 'Declarations' }, code: `@[dci_import("../contracts/Complex.dcib")]
extern "dci" {
    class abi_complex.AbstractSink {
        AbstractSink();
        virtual ~AbstractSink();
        virtual fn consume(x: i32) -> i32;
    };

    class abi_complex.NativeDriver {
        NativeDriver(bias: i32);
        ~NativeDriver();
        public bias: i32;
        public fn dispatch(sink: *abi_complex.AbstractSink,
                           x: i32) const -> i32;
    };
}` },
        { name: 'main.vyx · Vyx', language: 'Vyx', role: { zh: '继承与调用', en: 'Inherit & call' }, code: `class VyxSink : abi_complex.AbstractSink {
    VyxSink() {
        let init_marker = 0;
    }

    override fn consume(x: i32) -> i32 {
        return x + 7000;
    }
};

fn main() -> i32 {
    unsafe {
        var sink = VyxSink();
        let result = (sink as *abi_complex.AbstractSink).consume(1);
        print(result); // 7001
    }
    return 0;
}` },
      ],
    }],
  },
  {
    id: 'override',
    label: { zh: '原生回调覆写', en: 'Native override' },
    detail: { zh: 'Rust 动态分派回到 Vyx 方法', en: 'Rust dispatch calls a Vyx method' },
    output: 'native_dispatch(sink, 2) → 7032',
    source: 'tests/projects/dci_multilang/src/main.vyx',
    providers: [{
      id: 'rust', label: 'Rust', compiler: 'rustc', contract: 'native.dcib',
      files: [
        { name: 'lib.rs', language: 'Rust', role: { zh: '生产端定义', en: 'Producer' }, code: `pub trait Sink {
    fn consume(&self, x: i32) -> i32;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn native_dispatch(
    sink_obj: *const (), x: i32,
) -> i32 {
    unsafe {
        let vtable = *(sink_obj as *const *const ());
        let fat = [sink_obj as *const (), vtable];
        let sink: &dyn Sink = core::mem::transmute_copy(&fat);
        sink.consume(x) + 30
    }
}` },
        { name: 'main.vyx · DCI', language: 'Vyx', role: { zh: '契约声明', en: 'Declarations' }, code: `@[dci_import("../contracts/native.dcib")]
extern "dci" {
    class native.Sink {
        virtual fn consume(x: i32) -> i32;
    };

    fn native_dispatch(sink: rawptr, x: i32) -> i32;
}` },
        { name: 'main.vyx · Vyx', language: 'Vyx', role: { zh: '覆写与回调', en: 'Override & callback' }, code: `class VyxHost : native.Sink {
    VyxHost() {
        let init_marker = 0;
    }

    override fn consume(x: i32) -> i32 {
        return x + 7000;
    }
};

fn main() -> i32 {
    unsafe {
        var sink = VyxHost();
        let result = native_dispatch(sink as rawptr, 2);
        print(result); // Rust calls VyxHost.consume: 7032
    }
    return 0;
}` },
      ],
    }],
  },
  {
    id: 'open-generics',
    label: { zh: '开放泛型', en: 'Open generics' },
    detail: { zh: '调用方选择 Rust / C++ 类型实参', en: 'The caller selects Rust / C++ type arguments' },
    output: 'twice(21) → 42 · swap(21, 1.5).get_first() → 1.5',
    source: 'tests/projects/dci_opengeneric/',
    providers: dciProviders,
  },
]
