#![allow(dead_code, improper_ctypes_definitions)]

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Vec2 {
    pub x: f64,
    pub y: f64,
}

impl Vec2 {
    // 非泛型 record 的成员函数 —— 泛型 record 的成员函数（Pair2::get_first /
    // swapped）之外的对照片，与 cpp 侧 `?norm1@Vec2@@QEBANXZ` 是同一套调用点。
    // 适配器把它导成 `kind=method`、`owner=Vec2` 的**方法物化请求**（稳定链接名
    // `vyx_vec2_norm1`，semantic_id `dci.active.rust.Vec2::norm1()` —— 接收者不进
    // 实参列表，它是方法查询的隐式部分），构建期由 Active Adapter 生成 UFCS shim
    // 交给 rustc 单态化。rustc 不会替没人引用的函数发射符号，所以这里不能靠
    // `#[no_mangle]`，只能靠请求。
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
}
