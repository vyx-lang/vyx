"""Bounded, typed composition grammar and independent semantic interpreter.

Every production accepts an i64 observation, but introduces its own typed
intermediates (owners, references, enums, generic instances, etc.). A recipe
lists inner-to-outer productions. The interpreter never reads compiler output.
"""
from dataclasses import asdict, dataclass
import hashlib
import itertools
import json


@dataclass(frozen=True)
class Rule:
    name: str
    features: tuple[str, ...]
    input_type: str = "i64"
    output_type: str = "i64"
    precondition: str = "bounded signed arithmetic; no undefined behavior"


RULES = tuple(Rule(name, features) for name, features in [
    ("arithmetic", ("binary", "evaluation_order")),
    ("if_expr", ("if", "block_tail", "branch")),
    ("match_value", ("match", "value_arms")),
    ("tuple", ("tuple", "projection")),
    ("array", ("array", "index")),
    ("loop", ("while", "mutation")),
    ("generic", ("generic_function", "inference")),
    ("named_call", ("named_arguments", "generic_inference", "single_evaluation")),
    ("args_parameter", ("ordinary_parameter_name", "generic_inference", "named_arguments")),
    ("pipe_default", ("pipe_prefix", "default_arguments", "single_evaluation")),
    ("c_varargs", ("C_ellipsis", "named_arguments", "integer_promotion", "float_promotion", "libc_roundtrip")),
    ("class", ("aggregate", "receiver")),
    ("generic_class", ("generic_class", "static_factory", "receiver")),
    ("trait", ("interface", "impl", "generic_constraint", "shared_borrow")),
    ("enum", ("payload_enum", "match", "binding")),
    ("option", ("Option", "static_generic", "unwrap")),
    ("result", ("Result", "static_generic", "unwrap")),
    ("shared_ref", ("shared_borrow", "dereference")),
    ("mut_ref", ("mutable_borrow", "dereference", "mutation")),
    ("closure", ("closure", "capture", "mutation")),
    ("vec", ("Vec", "push", "index", "ownership")),
    ("borrowed_vec", ("Vec", "foreach", "shared_borrow")),
    ("string", ("String", "clone", "heap", "ownership")),
    ("builder", ("StringBuilder", "aggregate_return", "transfer", "reuse")),
    ("interpolation", ("interpolation", "nested_string_literal")),
    ("defer", ("defer", "scope_exit", "observable_effect")),
    ("drop", ("drop", "scope_exit", "observable_effect")),
    ("touch", ("call", "single_evaluation", "observable_effect")),
    ("global_widen", ("global", "i32", "implicit_widen", "storage")),
    ("narrow", ("i8", "explicit_cast", "signed_widen")),
    ("short_circuit", ("boolean", "short_circuit", "observable_effect")),
    ("return_cleanup", ("early_return", "drop", "defer", "observable_effect")),
    ("conditional_owner", ("if", "owned_branch_value", "drop")),
    ("plain_iterator", ("foreach", "declared_next_payload", "i64", "observable_effect")),
    ("generic_iterator", ("foreach", "generic_class", "second_type_argument", "observable_effect")),
    ("option_branch", ("Option", "Some", "None", "match")),
    ("result_branch", ("Result", "Ok", "Err", "match")),
    ("result_try", ("Result", "fail", "question_propagation", "drop", "defer")),
    ("option_try", ("Option", "None", "question_propagation", "drop", "defer")),
    ("array_foreach", ("array", "foreach", "shared_borrow", "i64")),
    ("pack_foreach", ("variadic_generic", "heterogeneous_pack", "foreach", "observable_effect")),
    ("own_iterator", ("foreach", "owned_payload", "break", "continue", "drop", "defer")),
    ("closure_loop", ("closure", "inferred_return", "while", "i64")),
    ("next_only_iterator", ("foreach", "Option_exhaustion", "next_only", "single_evaluation")),
    ("borrowed_owned_vec", ("Vec", "String", "Clone", "shared_borrow", "drop")),
    ("owned_dict", ("Dict", "String", "Clone", "replace", "remove", "drop")),
    ("owned_linear", ("Stack", "Queue", "String", "Clone", "move", "clear", "drop")),
    ("owned_array", ("array", "Clone", "shared_borrow", "drop", "single_evaluation")),
    ("inherited_iterator", ("foreach", "nominal_inheritance", "next_only", "Option_exhaustion")),
])
RULE_BY_NAME = {r.name: r for r in RULES}
TYPE_BASES = ("i64", "String", "Tracked")
TYPE_CONSTRUCTORS = ("Vec", "Option", "Result")
# This is an explicit scope boundary, not permission to ignore a failing rule.
UNMODELED = ["DCI producer ABIs", "async scheduling", "Effect schemas",
             "Reflection", "Migrate", "dynamic dispatch", "general unsafe pointers",
             "exceptions", "floating point", "separate library/VYI linking"]
NEGATIVES = {
    "moved_string": r"error: E3100:.*(?:moved|move|source)",
    "unknown_trait": r"error: E1100: unknown trait constraint.*MissingTrait",
    "nominal": r"error: E1000: call arg: type mismatch Right vs Left",
    "argument": r"error: E1000: call arg: type mismatch str vs i64",
    "scalar_foreach": r"error: E1000: cannot determine for-in element type",
    "question_return": r"error: E1000: question operator requires matching Result or Option",
    "moved_array": r"error: E3100:.*(?:moved|move)",
    "repeated_array": r"error: E3100: repeated array initialization cannot duplicate owning elements",
    "args_type": r"error: E1000: call arg: type mismatch str vs i64",
    "args_extra": r"error: E2200: no matching overload.*accepts at most 1 argument",
    "pipe_moved": r"error: E3100:.*(?:moved|move)",
}


def catalog():
    return {"version": 1, "productions": [asdict(r) for r in RULES],
            "type_grammar": {"bases": TYPE_BASES, "constructors": TYPE_CONSTRUCTORS,
                             "max_depth": 3, "observation": "construct owners, transfer payload out, observe i64 or String length"},
            "negatives": NEGATIVES, "unmodeled": UNMODELED,
            "max_depth": 6, "max_abs_leaf": 1000,
            "composition": "ordered, inner to outer; repetition allowed"}


def recipes(names, depth):
    yield ()
    for d in range(1, depth + 1):
        yield from itertools.product(names, repeat=d)


def case_id(case):
    identity = {k: case[k] for k in ("rules", "leaf", "form", "negative")}
    if case.get("type_recipe"): identity["type_recipe"] = case["type_recipe"]
    raw = json.dumps(identity,
                     sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(raw.encode()).hexdigest()[:16]


def remainder(value, divisor):
    # Vyx signed % truncates toward zero; Python % rounds toward -infinity.
    return value % divisor if value >= 0 else -((-value) % divisor)


def oracle(case):
    value = case["leaf"]
    calls = drops = defers = 0
    if case.get("type_recipe"):
        base = case["type_recipe"]["base"]
        if base == "String": value = len("heap-owned-payload-more-than-inline-buffer/" + str(value))
        elif base == "Tracked": drops = 1
        elif base != "i64": raise ValueError(f"unknown type base {base}")
    for rule in case["rules"]:
        if rule == "arithmetic": value = value * 3 + 2
        elif rule == "if_expr": value = value + 5 if value >= 0 else 3 - value
        elif rule == "match_value": value = 11 if value == 0 else value + 2
        elif rule == "tuple": value = value * 2 + 3
        elif rule == "array": value = value * 2 + 4
        elif rule == "loop": value += 6
        elif rule == "generic": value += 1
        elif rule == "named_call": value += 1; calls += 1
        elif rule == "args_parameter": value += 1; calls += 1
        elif rule == "pipe_default": value += 4; calls += 1
        elif rule == "c_varargs": calls += 1
        elif rule in ("class", "generic_class", "trait", "enum"): value += 2
        elif rule in ("option", "result", "shared_ref"): pass
        elif rule == "mut_ref": value += 4
        elif rule == "closure": value += 7
        elif rule in ("vec", "borrowed_vec"): value = value * 2 + 3
        elif rule in ("string", "builder", "interpolation"): value += len(str(value))
        elif rule == "defer": defers += 1
        elif rule == "drop": drops += 1
        elif rule == "touch": calls += 1; value += 1
        elif rule == "global_widen": value += 4
        elif rule == "narrow": value = remainder(value, 31)
        elif rule == "short_circuit": calls += 1
        elif rule == "return_cleanup": value += 2; drops += 1; defers += 1
        elif rule == "conditional_owner": value = value + 2 if value >= 0 else 3 - value; drops += 1
        elif rule in ("plain_iterator", "generic_iterator"): value = 2 * value + 8589934595; calls += 2
        elif rule == "option_branch": value = value + 1 if value >= 0 else -5
        elif rule == "result_branch": value = value + 2 if value >= 0 else -7
        elif rule == "result_try": value = value + 3 if value >= 0 else -7; defers += 1; drops += 1
        elif rule == "option_try": value = value + 3 if value >= 0 else -5; defers += 1; drops += 1
        elif rule == "array_foreach": value += 3
        elif rule == "pack_foreach": value += 3; calls += 3
        elif rule == "own_iterator":
            if value < 0: value += 1; calls += 1; drops += 1; defers += 1
            elif value == 0: value = 2; calls += 2; drops += 2; defers += 2
            else: value = value * 2 + 3; calls += 2; drops += 2; defers += 2
        elif rule == "closure_loop": value += 7
        elif rule == "next_only_iterator": value = 2 * value + 3; calls += 3
        elif rule == "borrowed_owned_vec": value += len(str(value)) + len("stable heap payload larger than inline storage")
        elif rule == "owned_dict": value = 3 * value + 6; calls += 3; drops += 6
        elif rule == "owned_linear": value = 3 * value + 2; calls += 3; drops += 6
        elif rule == "owned_array": value = 2 * value + 21; calls += 2; drops += 4
        elif rule == "inherited_iterator": value = 2 * value + 3; calls += 3
        else: raise ValueError(f"unknown production {rule}")
    return [value, calls, drops, defers, 0]


COMMON = '''use std.string;
use std.collections;
use std.core;
public var calls: i64 = 0;
public var drops: i64 = 0;
public var defers: i64 = 0;
public var rule_errors: i64 = 0;
public fn reset() { calls = 0; drops = 0; defers = 0; rule_errors = 0; }
public fn report(id: str, value: i64) {
    print(id, "/", value, "/", calls, "/", drops, "/", defers, "/", rule_errors);
}
'''
SUPPORT = {
    "owned_linear": '''use std.clone;
public class LinearOwned {
    public value: i64;
    public text: String;
    public fn drop(&mut self) { drops += 1; self.text.drop(); }
}
public fn linear_owned_value(value: i64) -> LinearOwned {
    return LinearOwned { value: value, text: String.from("linear container payload owns independent heap storage") };
}
impl Clone for LinearOwned {
    fn clone(&self) -> LinearOwned {
        calls += 1;
        return LinearOwned { value: self.value, text: self.text.clone() };
    }
}
public fn linear_owned_observation(value: i64) -> i64 {
    var total: i64 = 0;
    {
        var stack = Stack::<LinearOwned>.new();
        var queue = Queue::<LinearOwned>.new();
        stack.push(linear_owned_value(value));
        stack.push(linear_owned_value(value + 1));
        queue.enqueue(stack.pop());
        {
            let copy = stack.peek();
            if (copy.text.as_str() != "linear container payload owns independent heap storage") { rule_errors += 1; }
            total += copy.value;
        }
        stack.set(0, linear_owned_value(value + 2));
        {
            let copy = queue.get(0);
            if (copy.text.as_str() != "linear container payload owns independent heap storage") { rule_errors += 1; }
            total += copy.value;
        }
        {
            var snapshot = queue.clone();
            let item = snapshot.dequeue();
            if (item.text.as_str() != "linear container payload owns independent heap storage") { rule_errors += 1; }
            total += item.value;
        }
        stack.clear();
        queue.clear();
    }
    return total;
}
''',
    "owned_dict": '''use std.clone;
use std.defaults;
public class DictOwned {
    public value: i64;
    public text: String;
    public fn drop(&mut self) { drops += 1; self.text.drop(); }
}
public fn dict_owned_value(value: i64) -> DictOwned {
    return DictOwned { value: value, text: String.from("owned dictionary payload must remain independent beyond inline capacity") };
}
impl Default for DictOwned {
    static fn default_value() -> DictOwned { return dict_owned_value(0); }
}
impl Clone for DictOwned {
    fn clone(&self) -> DictOwned {
        calls += 1;
        return DictOwned { value: self.value, text: self.text.clone() };
    }
}
public fn dict_owned_observation(value: i64) -> i64 {
    var total: i64 = 0;
    {
        var values = Dict::<i64, DictOwned>.new();
        values.put(1, dict_owned_value(value));
        values.put(1, dict_owned_value(value + 2));
        {
            let copy = values.get(1);
            if (copy.text.as_str() != "owned dictionary payload must remain independent beyond inline capacity") { rule_errors += 1; }
            total += copy.value;
        }
        {
            match (values.try_get(1)) {
                Some(copy) => { total += copy.value; }
                None => { rule_errors += 1; }
            }
        }
        {
            let copies = values.values_vec();
            total += copies.get_ref(0).value;
        }
        if (!values.remove(1)) { rule_errors += 1; }
        values.put(2, dict_owned_value(value + 3));
    }
    return total;
}
''',
    "args_parameter": '''public fn args_identity<T>(args: T) -> T { return args; }
''',
    "pipe_default": '''public fn pipe_default_target(value: i64 = 999, delta: i64 = 3) -> i64 {
    return value + delta;
}
''',
    "c_varargs": '''extern "C" {
    fn snprintf(buffer: rawptr, size: u64, format: rawptr, ...) -> i32;
    fn strtoll(text: rawptr, end: rawptr, base: i32) -> i64;
}
public fn c_format_roundtrip(value: i64) -> i64 {
    calls += 1;
    var bytes: [u8; 64] = [0 as u8; 64];
    let negative: i8 = -7;
    let fraction: f32 = 1.5;
    unsafe {
        let count = snprintf(format: "%d/%.1f/%lld".c_str(), size: 64,
            buffer: to_rawptr(&bytes), negative, fraction, value);
        if (count < 8 || count >= 64 || bytes[0] != 45 || bytes[1] != 55
            || bytes[2] != 47 || bytes[3] != 49 || bytes[4] != 46
            || bytes[5] != 53 || bytes[6] != 47) { rule_errors += 1; }
        return strtoll(ptr_offset(to_rawptr(&bytes), 7), null, 10);
    }
}
''',
    "owned_array": '''public class ArrayOwned {
    public value: i64;
    public fn clone(&self) -> ArrayOwned { calls += 1; return ArrayOwned { value: self.value + 10 }; }
    public fn drop(&mut self) { drops += 1; }
}
public fn sum_owned_array(base: i64) -> i64 {
    let items = [ArrayOwned { value: base }, ArrayOwned { value: base + 1 }];
    var total: i64 = 0;
    for (item in &items) { total += item.value; }
    if (items[0].value != base || items[1].value != base + 1) { rule_errors += 1; }
    return total;
}
''',
    "inherited_iterator": '''public class InheritedPullBase {
    public base: i64;
    public pos: i64;
    public fn next() -> Option<i64> {
        calls += 1;
        if (self.pos >= 2) { return Option::<i64>.none(); }
        self.pos += 1;
        return Option::<i64>.some(self.base + self.pos);
    }
}
public class InheritedPull: InheritedPullBase { public extra: i64; }
public fn sum_inherited(base: i64) -> i64 {
    var total: i64 = 0;
    let cursor = InheritedPull { base: base, pos: 0, extra: 17 };
    for item in cursor { total += item; }
    return total;
}
''',
    "generic": "public fn identity<T>(value: T) -> T { return value; }\n",
    "named_call": "public fn named_identity<T>(value: T, enabled: bool) -> T { return value; }\n",
    "class": '''public class Cell {
    public value: i64;
    public fn get(&self) -> i64 { return self.value + 2; }
}
''',
    "generic_class": '''public class GenericCell<T> {
    public value: T;
    public static fn make(value: T) -> GenericCell<T> { return GenericCell::<T> { value: value }; }
    public fn get(&self) -> T { return self.value; }
}
''',
    "trait": '''public interface Metric { fn measure(&self) -> i64; }
public class Measured { public value: i64; }
impl Metric for Measured { fn measure(&self) -> i64 { return self.value + 2; } }
public fn measure<T>(value: &T) -> i64 where T: Metric { return value.measure(); }
''',
    "enum": "public enum Payload { Empty, Value(i64), }\n",
    "shared_ref": "public fn read_ref(value: &i64) -> i64 { return *value; }\n",
    "mut_ref": "public fn add_ref(value: &mut i64) { *value = *value + 4; }\n",
    "borrowed_vec": '''public fn sum_ref(values: &Vec<i64>) -> i64 {
    var sum: i64 = 0;
    for (value in values) { sum = sum + value; }
    return sum;
}
''',
    "drop": '''public class Tracked {
    public value: i64;
    public fn drop(&mut self) { drops = drops + 1; }
    public fn get(&self) -> i64 { return self.value; }
}
''',
    "touch": "public fn touch(value: i64) -> i64 { calls = calls + 1; return value + 1; }\n",
    "global_widen": '''public let offset: i32 = 4;
public let poison: i32 = 1717986918;
public fn widen(value: i64) -> i64 { return value; }
''',
    "return_cleanup": '''public fn cleanup_return(value: i64) -> i64 {
    defer { defers = defers + 1; }
    let owner = Tracked { value: value };
    if (value < 0) { return owner.get() + 2; }
    return owner.get() + 2;
}
''',
    "own_iterator": '''public class OwningCursor {
    public base: i64;
    public pos: i64;
    public fn has_next() -> bool { return self.pos < 2; }
    public fn next() -> Option<Tracked> {
        self.pos += 1; calls += 1;
        return Option::<Tracked>.some(Tracked { value: self.base + self.pos });
    }
}
public fn sum_owned(base: i64) -> i64 {
    var total: i64 = 0;
    for item in (OwningCursor { base: base, pos: 0 }) {
        defer { defers += 1; }
        if (base == 0 && item.get() == 1) { continue; }
        total += item.get();
        if (base < 0) { break; }
    }
    return total;
}
''',
    "next_only_iterator": '''public class PullCursor {
    public base: i64;
    public pos: i64;
    public fn next() -> Option<i64> {
        calls += 1;
        if (self.pos >= 2) { return Option::<i64>.none(); }
        self.pos += 1;
        return Option::<i64>.some(self.base + self.pos);
    }
}
public fn sum_pull(base: i64) -> i64 {
    var total: i64 = 0;
    for item in (PullCursor { base: base, pos: 0 }) { total += item; }
    return total;
}
''',
    "plain_iterator": '''public class WideCursor {
    public first: i64;
    public second: i64;
    public pos: i32;
    public fn has_next() -> bool { return self.pos < 2; }
    public fn next() -> Option<i64> {
        calls = calls + 1;
        if (self.pos == 0) { self.pos = 1; return Option::<i64>.some(self.first); }
        if (self.pos == 1) { self.pos = 2; return Option::<i64>.some(self.second); }
        return Option::<i64>.none();
    }
}
''',
    "generic_iterator": '''public class TaggedCursor<Tag, Item> {
    public first: Item;
    public second: Item;
    public pos: i32;
    public fn has_next() -> bool { return self.pos < 2; }
    public fn next() -> Option<Item> {
        calls = calls + 1;
        if (self.pos == 0) { self.pos = 1; return Option::<Item>.some(self.first); }
        if (self.pos == 1) { self.pos = 2; return Option::<Item>.some(self.second); }
        return Option::<Item>.none();
    }
}
''',
    "result_try": '''public error CheckError { Rejected }
public fn checked(value: i64) -> Result<i64, CheckError> {
    if (value < 0) { fail CheckError.Rejected; }
    return value + 1;
}
public fn propagated(value: i64) -> Result<i64, CheckError> {
    let owner = Tracked { value: value };
    defer { defers = defers + 1; }
    let result = checked(value)?;
    return result + 2 + (owner.get() - value);
}
''',
    "option_try": '''public fn optional_checked(value: i64) -> Option<i64> {
    if (value < 0) { return Option::<i64>.none(); }
    return Option::<i64>.some(value + 1);
}
public fn optional_propagated(value: i64) -> Option<i64> {
    let owner = Tracked { value: value };
    defer { defers = defers + 1; }
    let result = optional_checked(value)?;
    return Option::<i64>.some(result + 2 + (owner.get() - value));
}
''',
    "array_foreach": '''public fn sum_array_ref(values: &[i64; 3]) -> i64 {
    var total: i64 = 0;
    for value in values { total = total + value; }
    return total;
}
''',
    "pack_foreach": '''public fn sum_pack<...Ts>(...values: Ts) -> i64 {
    var total: i64 = 0;
    for value in values { calls = calls + 1; total = total + (value as i64); }
    return total;
}
''',
}
BAD_SUPPORT = {
    "moved_string": "",
    "unknown_trait": "public fn unproven<T>(value: T) -> T where T: MissingTrait { return value; }\n",
    "nominal": '''public class Left { public value: i64; }
public class Right { public value: i64; }
public fn take_left(value: Left) -> i64 { return value.value; }
''',
    "argument": "public fn take_number(value: i64) -> i64 { return value; }\n",
    "scalar_foreach": "",
    "question_return": "",
    "moved_array": "",
    "repeated_array": "",
    "args_type": "public fn scalar_args(args: i64) -> i64 { return args; }\n",
    "args_extra": "public fn scalar_args(args: i64) -> i64 { return args; }\n",
    "pipe_moved": "public fn consume_pipe_value(value: String, enabled: bool = true) { }\n",
}


class Emitter:
    def __init__(self, case):
        self.case = case
        self.serial = 0
        self.lines = []

    def name(self, prefix="v"):
        self.serial += 1
        # A composed program can reach serial 8/16/32/64. `i8` is a
        # reserved primitive type, so all generated symbols use a separator.
        return f"{prefix}_{self.serial}"

    def bind(self, expr, mutable=False):
        name = self.name()
        self.lines.append(f"{'var' if mutable else 'let'} {name}: i64 = {expr};")
        return name

    def typed_value(self):
        recipe = self.case["type_recipe"]
        typ = recipe["base"]
        obj = self.name("payload")
        value = self.case["leaf"]
        if typ == "i64": initializer = f"({value} as i64)"
        elif typ == "String": initializer = f'String.from("heap-owned-payload-more-than-inline-buffer/{value}")'
        elif typ == "Tracked": initializer = f"Tracked {{ value: {value} }}"
        else: raise ValueError(f"unknown type base {typ}")
        self.lines.append(f"let {obj}: {typ} = {initializer};")
        types = [typ]
        for ctor in recipe["constructors"]:
            outer = self.name("outer")
            if ctor == "Vec":
                self.lines += [f"var {outer} = Vec::<{typ}>.new();", f"{outer}.push({obj});"]
                typ = f"Vec<{typ}>"
            elif ctor == "Option":
                self.lines.append(f"let {outer} = Option::<{typ}>.some({obj});")
                typ = f"Option<{typ}>"
            elif ctor == "Result":
                self.lines.append(f"let {outer} = Result::<{typ}, i32>.ok({obj});")
                typ = f"Result<{typ}, i32>"
            else: raise ValueError(f"unknown type constructor {ctor}")
            obj = outer
            types.append(typ)
        for index in reversed(range(len(recipe["constructors"]))):
            ctor = recipe["constructors"][index]
            inner = self.name("taken")
            if ctor == "Vec": expression = f"{obj}.pop()"
            elif ctor in ("Option", "Result"): expression = f"{obj}.unwrap()"
            else: raise ValueError(f"unknown type constructor {ctor}")
            self.lines.append(f"var {inner}: {types[index]} = {expression};")
            obj = inner
        if recipe["base"] == "String": expression = f"{obj}.len"
        elif recipe["base"] == "Tracked": expression = f"{obj}.get()"
        else: expression = obj
        return expression, oracle({**self.case, "rules": []})[0]

    def emit(self):
        expr = f"({self.case['leaf']} as i64)"
        # Embed the invalid operation at the AST leaf, not in a permanently
        # invalid helper body. Outer rules really supply the use-site context.
        if self.case["negative"] == "moved_string":
            self.lines += ['let source = String.from("owned text larger than an inline string buffer");',
                           'let destination = source;']
            expr = f"({expr} + source.len + destination.len)"
        elif self.case["negative"] == "unknown_trait": expr = f"unproven({expr})"
        elif self.case["negative"] == "nominal": expr = f"take_left(Right {{ value: {expr} }})"
        elif self.case["negative"] == "argument": expr = 'take_number("not a number")'
        elif self.case["negative"] == "scalar_foreach":
            self.lines.append(f"for invalid_item in {expr} {{ }}")
        elif self.case["negative"] == "question_return":
            expr = f"Option::<i64>.some({expr})?"
        elif self.case["negative"] == "moved_array":
            self.lines += [f"let array_owner = [Tracked {{ value: {expr} }}];", "let array_destination = array_owner;"]
            expr = "(array_owner[0].get() + array_destination[0].get())"
        elif self.case["negative"] == "repeated_array":
            self.lines.append(f"let array_repeat = [Tracked {{ value: {expr} }}; 2];")
            expr = "array_repeat[0].get()"
        elif self.case["negative"] == "args_type": expr = 'scalar_args("not a number")'
        elif self.case["negative"] == "args_extra": expr = f"scalar_args({expr}, 2 as i64)"
        elif self.case["negative"] == "pipe_moved":
            self.lines += ['let pipe_owner = String.from("owned text larger than an inline string buffer");',
                           'pipe_owner |> consume_pipe_value();']
            expr = f"({expr} + pipe_owner.len)"
        value = self.case["leaf"]
        if self.case.get("type_recipe"): expr, value = self.typed_value()
        for rule in self.case["rules"]:
            if rule not in RULE_BY_NAME: raise ValueError(f"unknown production {rule}")
            if self.case["form"] == "bound": expr = self.bind(expr)
            if rule == "arithmetic": expr = f"(({expr}) * 3 + 2)"
            elif rule == "if_expr":
                x = self.bind(expr)
                expr = f"(if ({x} >= 0) {{ {x} + 5 }} else {{ 3 - {x} }})"
            elif rule == "match_value":
                x = self.bind(expr)
                expr = f"(match {x} {{ case 0 => 11 as i64, default => {x} + 2, }})"
            elif rule in ("tuple", "array"):
                x, obj = self.bind(expr), self.name("pack")
                if rule == "tuple":
                    self.lines.append(f"let {obj} = ({x}, {x} + 3);")
                    expr = f"({obj}.0 + {obj}.1)"
                else:
                    self.lines.append(f"let {obj}: [i64; 2] = [{x}, {x} + 4];")
                    expr = f"({obj}[0] + {obj}[1])"
            elif rule == "loop":
                x, i = self.bind(expr, True), self.name("i")
                self.lines += [f"var {i}: i64 = 0;",
                               f"while ({i} < 3) {{ {i} = {i} + 1; {x} = {x} + {i}; }}"]
                expr = x
            elif rule == "generic": expr = f"(identity({expr}) + 1)"
            elif rule == "named_call": expr = f"named_identity(enabled: true, value: touch({expr}))"
            elif rule == "args_parameter": expr = f"args_identity(args: touch({expr}))"
            elif rule == "pipe_default": expr = f"(touch({expr}) |> pipe_default_target())"
            elif rule == "c_varargs": expr = f"c_format_roundtrip({expr})"
            elif rule == "class":
                obj = self.name("cell")
                self.lines.append(f"let {obj} = Cell {{ value: {expr} }};")
                expr = f"{obj}.get()"
            elif rule == "generic_class": expr = f"(GenericCell::<i64>.make({expr}).get() + 2)"
            elif rule == "trait":
                obj = self.name("metric")
                self.lines.append(f"let {obj} = Measured {{ value: {expr} }};")
                expr = f"measure(&{obj})"
            elif rule == "enum":
                obj = self.name("payload")
                self.lines.append(f"let {obj} = Payload.Value({expr});")
                expr = f"(match {obj} {{ case Value(x) => x + 2, default => 0 as i64, }})"
            elif rule == "option": expr = f"Option::<i64>.some({expr}).unwrap()"
            elif rule == "result": expr = f"Result::<i64, i32>.ok({expr}).unwrap()"
            elif rule == "shared_ref": expr = f"read_ref(&{self.bind(expr)})"
            elif rule == "mut_ref":
                expr = self.bind(expr, True)
                self.lines.append(f"add_ref(&mut {expr});")
            elif rule == "closure":
                x, closure = self.bind(expr, True), self.name("closure")
                self.lines.append(f"let {closure} = [&{x}](n: i64) => {{ {x} = {x} + n; return {x}; }};")
                self.lines += [f"{closure}(3);", f"{closure}(4);"]
                expr = x
            elif rule in ("vec", "borrowed_vec"):
                x, obj = self.bind(expr), self.name("vec")
                self.lines += [f"var {obj} = Vec::<i64>.new();", f"{obj}.push({x});", f"{obj}.push({x} + 3);"]
                # Vec.operator[] returns &mut T, unlike array indexing. Read
                # those elements explicitly rather than assuming ref arithmetic.
                expr = f"(*{obj}[0] + *{obj}[1])" if rule == "vec" else f"sum_ref(&{obj})"
            elif rule == "string":
                x, obj, twin = self.bind(expr), self.name("text"), self.name("clone")
                # Python independently derives decimal digits; guard is observable
                # even when an outer rule discards/modulos the numeric result.
                self.lines += [f'let {obj} = String.from("long-owned-value/{value}");',
                               f"let {twin} = {obj}.clone();",
                               f'if ({twin}.as_str() != "long-owned-value/{value}") {{ rule_errors = rule_errors + 1; }}']
                expr = f"({x} + {twin}.len - 17)"
            elif rule == "builder":
                x, obj, text, reuse = self.bind(expr), self.name("builder"), self.name("text"), self.name("reuse")
                self.lines += [f"var {obj} = StringBuilder.withCapacity(128);", f"{obj}.appendI64({x});",
                               f"let {text} = {obj}.intoString();", f'{obj}.append("reuse");',
                               f"let {reuse} = {obj}.build();", f"{obj}.destroy();",
                               f'if ({text}.as_str() != "{value}" || {reuse}.as_str() != "reuse") {{ rule_errors = rule_errors + 1; }}']
                expr = f"({x} + {text}.len)"
            elif rule == "interpolation":
                x, obj = self.bind(expr), self.name("text")
                self.lines += [f'let {obj} = "${{{x}}}/${{String.from(\\"x\\").len}}";',
                               f'if ({obj} != "{value}/1") {{ rule_errors = rule_errors + 1; }}']
                expr = f"({x} + {obj}.len - 2)"
            elif rule == "defer":
                x = self.bind(expr, True)
                self.lines.append(f"{{ defer {{ defers = defers + 1; }} {x} = {x} + 0; }}")
                expr = x
            elif rule == "drop":
                x, obj = self.bind(expr, True), self.name("owner")
                self.lines.append(f"{{ let {obj} = Tracked {{ value: {x} }}; {x} = {obj}.get(); }}")
                expr = x
            elif rule == "touch": expr = f"touch({expr})"
            elif rule == "global_widen": expr = f"(({expr}) + widen(offset))"
            elif rule == "narrow":
                x, obj = self.bind(expr), self.name("narrow")
                self.lines.append(f"let {obj}: i8 = ({x} % 31) as i8;")
                expr = f"({obj} as i64)"
            elif rule == "short_circuit":
                x = self.bind(expr)
                self.lines += [f"if (false && touch({x}) == {x}) {{ rule_errors = rule_errors + 1; }}",
                               f"if (!(true || touch({x}) == {x})) {{ rule_errors = rule_errors + 1; }}",
                               f"if (!(true && touch({x}) == {x} + 1)) {{ rule_errors = rule_errors + 1; }}"]
                expr = x
            elif rule == "return_cleanup": expr = f"cleanup_return({expr})"
            elif rule == "conditional_owner":
                x, obj = self.bind(expr), self.name("selected")
                self.lines.append(f"let {obj} = if ({x} >= 0) {{ Tracked {{ value: {x} + 2 }} }} else {{ Tracked {{ value: 3 - {x} }} }};")
                expr = f"{obj}.get()"
            elif rule in ("plain_iterator", "generic_iterator"):
                x, obj, total = self.bind(expr), self.name("cursor"), self.name("sum")
                typ = "WideCursor" if rule == "plain_iterator" else "TaggedCursor::<u8, i64>"
                self.lines += [f"var {obj} = {typ} {{ first: {x} + 4294967296, second: {x} + 4294967299, pos: 0 }};",
                               f"var {total}: i64 = 0;", f"for (item in {obj}) {{ {total} = {total} + item; }}"]
                expr = total
            elif rule == "option_branch":
                x, obj = self.bind(expr), self.name("selected")
                self.lines.append(f"let {obj} = if ({x} >= 0) {{ Option::<i64>.some({x} + 1) }} else {{ Option::<i64>.none() }};")
                expr = f"(match {obj} {{ case Some(v) => v, default => -5 as i64, }})"
            elif rule == "result_branch":
                x, obj = self.bind(expr), self.name("selected")
                self.lines.append(f"let {obj} = if ({x} >= 0) {{ Result::<i64, i32>.ok({x} + 2) }} else {{ Result::<i64, i32>.err(7) }};")
                expr = f"(match {obj} {{ case Ok(v) => v, case Err(e) => -(e as i64), }})"
            elif rule == "result_try":
                obj = self.name("result")
                self.lines.append(f"let {obj} = propagated({expr});")
                expr = f"(match {obj} {{ case Ok(v) => v, case Err(_) => -7 as i64, }})"
            elif rule == "option_try":
                obj = self.name("result")
                self.lines.append(f"let {obj} = optional_propagated({expr});")
                expr = f"(match {obj} {{ case Some(v) => v, default => -5 as i64, }})"
            elif rule == "array_foreach":
                x, obj, total = self.bind(expr), self.name("items"), self.name("sum")
                self.lines += [f"let {obj} = [{x}, 2 as i64, 1 as i64];", f"var {total}: i64 = 0;",
                               f"for item in {obj} {{ {total} = {total} + item; }}",
                               f"if ({total} != sum_array_ref(&{obj})) {{ rule_errors = rule_errors + 1; }}"]
                expr = total
            elif rule == "pack_foreach":
                expr = f"(sum_pack({expr}, 2 as u8, 1 as i32) + sum_pack())"
            elif rule == "own_iterator": expr = f"sum_owned({expr})"
            elif rule == "next_only_iterator": expr = f"sum_pull({expr})"
            elif rule == "owned_array": expr = f"sum_owned_array({expr})"
            elif rule == "inherited_iterator": expr = f"sum_inherited({expr})"
            elif rule == "owned_dict": expr = f"dict_owned_observation({expr})"
            elif rule == "owned_linear": expr = f"linear_owned_observation({expr})"
            elif rule == "borrowed_owned_vec":
                x, obj, total = self.bind(expr), self.name("strings"), self.name("lengths")
                stable = "stable heap payload larger than inline storage"
                self.lines += [f"var {obj} = Vec::<String>.new();", f'{obj}.push(String.from("{value}"));',
                    f'{obj}.push(String.from("{stable}"));', f"var {total}: i64 = 0;",
                    f"for (item in &{obj}) {{ {total} += item.len; }}",
                    f'if ({obj}.get_ref(0).as_str() != "{value}" || {obj}.get_ref(1).as_str() != "{stable}") {{ rule_errors += 1; }}']
                expr = f"({x} + {total})"
            elif rule == "closure_loop":
                obj = self.name("closure")
                self.lines.append(f"let {obj} = [](input: i64) => {{ while (true) {{ return input + 7; }} }};")
                expr = f"{obj}({expr})"
            value = oracle({**self.case, "leaf": value, "rules": [rule]})[0]
        return "\n".join(self.lines + [f"return {expr};"])


def support(cases):
    used = {r for c in cases for r in c["rules"]}
    if used & {"return_cleanup", "conditional_owner", "result_try", "option_try", "own_iterator"}: used.add("drop")
    if "short_circuit" in used: used.add("touch")
    if used & {"named_call", "args_parameter", "pipe_default"}: used.add("touch")
    if any(c.get("type_recipe", {}).get("base") == "Tracked" for c in cases): used.add("drop")
    bad = {c["negative"] for c in cases if c["negative"]}
    if bad & {"moved_array", "repeated_array"}: used.add("drop")
    return COMMON + "".join(SUPPORT.get(r.name, "") for r in RULES if r.name in used) + "".join(BAD_SUPPORT[n] for n in sorted(bad))


def case_functions(cases):
    return "\n".join(f"public fn case_{c['id']}() -> i64 {{\n{Emitter(c).emit()}\n}}" for c in cases) + "\n"


def main(cases):
    return "fn main() -> i32 {\n" + "\n".join(
        f'reset(); let result_{c["id"]} = case_{c["id"]}(); report("{c["id"]}", result_{c["id"]});'
        for c in cases) + "\nreturn 0;\n}\n"


def expected(cases):
    return "".join(c["id"] + "/" + "/".join(map(str, oracle(c))) + "\n" for c in cases)
