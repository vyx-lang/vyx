pub const Pair = extern struct {
    left: i32,
    right: i32,
};

pub const Native = struct {
    flag: u8,
    wide: u64,
};

extern "c" fn malloc(size: usize) ?*anyopaque;
extern "c" fn free(ptr: ?*anyopaque) void;

export fn pair_sum(value: Pair) i32 {
    return value.left + value.right;
}

export fn pair_make(left: i32, right: i32) Pair {
    return .{ .left = left, .right = right };
}

export fn c_mul(a: i32, b: i32) i32 {
    return a * b;
}

export fn native_flag(value: *const Native) u8 {
    return value.flag;
}

export fn native_heap() *Native {
    const raw = malloc(@sizeOf(Native)) orelse unreachable;
    const p: *Native = @ptrCast(@alignCast(raw));
    p.* = .{ .flag = 9, .wide = 1 };
    return p;
}

export fn native_free(p: *Native) void {
    free(p);
}
