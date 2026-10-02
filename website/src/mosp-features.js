const shippingV1 = `@[version("1.0.0")]
@[variant("standard")]
module Shipping;

public fn quote(weight_kg: i32) -> i32 {
    return 8 + weight_kg * 2;
}`

const shippingV2 = `@[version("2.0.0")]
@[variant("standard")]
module Shipping;

@[migrate(fromVer="1.0.0", fromSig=fn quote(i32)->i32)]
public fn quote(weight_kg: i32) -> i32 {
    return 5 + weight_kg * 2;
}`

const shippingMain = `use Shipping;

fn main() -> i32 {
    print(quote(3));
    print(quote@1.0.0["standard"](3));
    return 0;
}`

const reflectionCounter = `use std.reflect;

@[reflect("Counter")]
public class Counter {
    public value: i32;

    @[reflect(alias="read")]
    public fn current() -> i32 {
        return self.value;
    }

    @[hidden]
    public fn reset() {
        self.value = 0;
    }
}

fn main() -> i32 {
    var counter = Counter { value: 42 };
    let counter_type = getType("Counter");
    var instance = counter_type.bind(&counter);
    let method = instance.getMethod("read");
    let read = method.as::<fn()->i32>();
    print(read());
    return 0;
}`

const reflectionMeter = `use std.reflect;

@[reflect("MeterType", alias=["MeterAlias"])]
public class Meter {
    @[reflect("value", alias=["reading"])]
    public value: i64;

    public fn bump(delta: i32) -> i32 {
        return (self.value as i32) + delta;
    }
}

type BoundBump = fn(i32) -> i32;

fn main() -> i32 {
    var src = Meter { value: 7 };
    let t = getType("MeterAlias");
    if (!t.valid) { return 1; }
    if (t.name != "MeterType") { return 2; }
    if (!t.matches::<Meter>()) { return 3; }

    let field = t.getField("reading");
    if (!field.valid || field.name != "value") { return 4; }
    let at0 = t.getFieldAt(0);
    if (!at0.valid || at0.name != "value") { return 5; }

    var inst = t.bind(&src);
    if (!inst.valid) { return 6; }
    let prop = inst.getProperty("reading");
    if (!prop.valid || prop.view::<i64>() != 7) { return 7; }
    inst.write::<i64>(prop, 11);
    if (src.value != 11) { return 8; }

    let m = inst.getMethod("bump");
    if (!m.valid || m.param_count != 1) { return 9; }
    let bound: BoundBump = m.as::<BoundBump>();
    if (bound(1) != 12) { return 10; }
    return 0;
}`

export const mospFeatures = {
  zh: {
    migrate: {
      title: '模块版本与历史调用',
      intro: 'Migrate 将当前声明与历史声明关联起来。项目可以同时提供多个模块版本，普通调用使用选定版本，也可以在调用处指定旧版本。',
      points: [
        { name: '版本与变体', detail: '@[version] 标识模块版本，@[variant] 区分同版本的不同实现。Vyx.lock 记录默认选择。' },
        { name: '历史调用', detail: 'quote@1.0.0["standard"](3) 明确选择历史实现；不带版本的 quote(3) 使用锁文件选定的实现。' },
        { name: '声明迁移', detail: '@[migrate] 用 fromVer 指定来源版本；fromSig 关联历史函数签名，fromField 关联改名前的字段。编译器检查这些声明关系。' },
      ],
      examples: [
        { filename: 'src/shipping_v1.vyx', description: '第一版：基础运费 8，每公斤加 2。quote(3) 返回 14。', code: shippingV1 },
        { filename: 'src/shipping_v2.vyx', description: '第二版：基础运费改为 5，并标记它与第一版 quote 的迁移关系。', code: shippingV2 },
        { filename: 'src/main.vyx', description: '锁文件选择第二版时，两个调用分别输出 11 和 14。', code: shippingMain },
      ],
      scopeTitle: '项目需要提供对应版本',
      scope: '将三个文件放在同一项目中，在 Vyx.lock 写入 Shipping:2.0.0["standard"]；完整 Vyx.toml 见示例文档。这些声明记录程序版本的关系，应用数据的迁移由业务代码处理。',
      links: [
        { title: '完整运费示例', description: '目录、清单、锁文件与运行命令。', path: 'docs/MOSP_ZH.md' },
        { title: '字段与历史方法', description: '字段改名、构造函数与历史方法的完整用法。', path: 'tests/projects/tutorial_migrate/src/main.vyx' },
        { title: '项目配置', description: '模块版本、目标与锁文件配置。', path: 'docs/PACKAGE_MANIFEST_ZH.md' },
      ],
    },
    reflection: {
      title: '按名称发现类型，绑定实例和成员',
      intro: 'Reflection 使用编译器生成的类型信息。通过 std.reflect，可以查找登记的类型和别名，绑定已有实例，读取字段或取得可调用的方法。',
      points: [
        { name: '登记名称', detail: '@[reflect] 为类型和成员登记名称、别名；@[hidden] 排除成员。示例将 current 方法登记为 read。' },
        { name: '查找与检查', detail: 'getType、getField、getMethod 返回描述对象。名称来自外部输入时，应检查 valid；matches::<T>() 可核对类型。' },
        { name: '绑定与调用', detail: 'bind(&object) 绑定现有实例，不转移所有权。getMethod 找到方法后，用 as::<fn(...) -> R>() 指明调用签名。' },
        { name: '字段读写', detail: 'getProperty 取得绑定实例的字段。view::<T>() 读取，write::<T>() 写入；Meter 示例展示对原对象字段的读写。' },
      ],
      examples: [
        { filename: 'reflection.vyx', description: '绑定 Counter，再通过 read 别名调用 current，输出 42。reset 被排除在反射成员之外。', code: reflectionCounter },
        { filename: 'tutorial_reflect.vyx', description: '按类型和字段别名查找，将原字段从 7 写为 11，再绑定 bump(1) 得到 12。', code: reflectionMeter },
      ],
      scopeTitle: '保持实例有效，使用匹配的类型',
      scope: '绑定期间保持原实例存活，字段类型与方法签名须匹配。运行时反射需要保留元数据和可调用代码，这些入口参与 DCE。T::name、T::fields 等编译期查询是另一条使用路径。',
      links: [
        { title: '反射用法', description: '名称登记、实例绑定与方法调用。', path: 'docs/MOSP_ZH.md' },
        { title: '字段和方法示例', description: '本页 Meter 示例的完整源文件。', path: 'tests/cases/tutorial_reflect.vyx' },
        { title: '继承与接口信息', description: '父类、接口、隐藏字段和绑定方法的完整示例。', path: 'tests/cases/reflection_full_model.vyx' },
      ],
    },
    dce: {
      title: '可达性与代码裁剪',
      intro: 'DCE（Dead Code Elimination，死代码消除）利用调用关系和程序信息判断哪些代码需要留下。开发者无需为它逐个标记函数。',
      points: [
        { name: '确定入口', detail: '从程序入口、导出接口和反射登记中找出必须保留的代码。' },
        { name: '追踪依赖', detail: '沿调用和引用关系追踪可达内容，按需构建函数体。' },
        { name: '裁剪与生成', detail: '在 MIR 中移除无用代码，保留必要行为，再交给 LLVM 优化和生成机器码。' },
      ],
      scopeTitle: '没有直接调用，也可能需要保留',
      scope: '反射调用、导出接口、析构和可观察副作用都会影响保留决定。裁剪范围取决于编译器掌握的信息与优化设置。',
      links: [
        { title: 'MOSP 中的 DCE', description: '可达性、反射入口与必要行为。', path: 'docs/MOSP_ZH.md' },
        { title: '编译流程', description: '函数体物化、MIR 优化和 LLVM 代码生成。', path: 'docs/COMPILER_ZH.md' },
      ],
    },
  },
  en: {
    migrate: {
      title: 'Module versions and historical calls',
      intro: 'Migrate connects current declarations to historical declarations. A project can provide several module versions. Ordinary calls use the selected version; a call can also specify an older version.',
      points: [
        { name: 'Versions and variants', detail: '@[version] identifies a module revision; @[variant] distinguishes implementations of that revision. Vyx.lock records the default selection.' },
        { name: 'Historical calls', detail: 'quote@1.0.0["standard"](3) selects the historical implementation. Unversioned quote(3) uses the implementation selected by the lockfile.' },
        { name: 'Declaration migration', detail: '@[migrate] identifies the source revision with fromVer. fromSig connects a historical function signature; fromField connects a renamed field. The compiler checks these declaration relationships.' },
      ],
      examples: [
        { filename: 'src/shipping_v1.vyx', description: 'Version one: a base fee of 8 plus 2 per kilogram. quote(3) returns 14.', code: shippingV1 },
        { filename: 'src/shipping_v2.vyx', description: 'Version two changes the base fee to 5 and records its migration relationship to the first quote.', code: shippingV2 },
        { filename: 'src/main.vyx', description: 'With version two selected in the lockfile, these calls print 11 and 14.', code: shippingMain },
      ],
      scopeTitle: 'Provide the corresponding module versions',
      scope: 'Put all three files in one project and set Shipping:2.0.0["standard"] in Vyx.lock. The linked example includes Vyx.toml. These declarations record relationships between program versions; application code handles data migration.',
      links: [
        { title: 'Complete shipping example', description: 'Directories, manifest, lockfile and run command.', path: 'docs/MOSP.md' },
        { title: 'Fields and historical methods', description: 'Complete usage of field renaming, constructors and historical methods.', path: 'tests/projects/tutorial_migrate/src/main.vyx' },
        { title: 'Project configuration', description: 'Module versions, targets and lockfile configuration.', path: 'docs/PACKAGE_MANIFEST.md' },
      ],
    },
    reflection: {
      title: 'Discover types by name and bind instances and members',
      intro: 'Reflection uses compiler-generated type information. std.reflect can find registered types and aliases, bind existing instances, read fields and obtain callable methods.',
      points: [
        { name: 'Register names', detail: '@[reflect] registers names and aliases for types and members. @[hidden] excludes members. The example registers current under the alias read.' },
        { name: 'Look up and check', detail: 'getType, getField and getMethod return descriptors. Check valid for names supplied externally; matches::<T>() can verify a type.' },
        { name: 'Bind and call', detail: 'bind(&object) attaches to an existing instance without transferring ownership. After getMethod finds a method, as::<fn(...) -> R>() specifies its call signature.' },
        { name: 'Read and write fields', detail: 'getProperty accesses a field on a bound instance. view::<T>() reads and write::<T>() writes. The Meter example reads and writes fields on the original object.' },
      ],
      examples: [
        { filename: 'reflection.vyx', description: 'Bind Counter and call current through its read alias to print 42. reset is excluded from reflection.', code: reflectionCounter },
        { filename: 'tutorial_reflect.vyx', description: 'Find a type and field by alias, change the original field from 7 to 11, then bind bump(1) to obtain 12.', code: reflectionMeter },
      ],
      scopeTitle: 'Keep the instance alive and use matching types',
      scope: 'Keep the original instance alive while bound. Field types and method signatures must match. Runtime reflection retains metadata and callable code, and those entries participate in DCE. Compile-time queries such as T::name and T::fields are a separate path.',
      links: [
        { title: 'Reflection usage', description: 'Name registration, instance binding and method calls.', path: 'docs/MOSP.md' },
        { title: 'Fields and methods example', description: 'The complete source of this page’s Meter example.', path: 'tests/cases/tutorial_reflect.vyx' },
        { title: 'Inheritance and interface information', description: 'A complete example of parents, interfaces, hidden fields and bound methods.', path: 'tests/cases/reflection_full_model.vyx' },
      ],
    },
    dce: {
      title: 'Reachability and code elimination',
      intro: 'DCE (Dead Code Elimination) uses call relationships and program information to decide which code needs to remain. Developers do not need to annotate individual functions for it.',
      points: [
        { name: 'Find entry points', detail: 'Identify required code from the program entry, exported interfaces and reflection registrations.' },
        { name: 'Follow dependencies', detail: 'Trace calls and references to reachable code and materialize function bodies as needed.' },
        { name: 'Eliminate and generate', detail: 'Remove unused code in MIR while retaining required behavior, then pass it to LLVM for optimization and machine code generation.' },
      ],
      scopeTitle: 'Code without a direct call may still be needed',
      scope: 'Reflection calls, exported interfaces, destruction and observable side effects all affect retention. Elimination depends on the information available to the compiler and the optimization settings.',
      links: [
        { title: 'DCE within MOSP', description: 'Reachability, reflection entries and required behavior.', path: 'docs/MOSP.md' },
        { title: 'Compilation flow', description: 'Function materialization, MIR optimization and LLVM code generation.', path: 'docs/COMPILER.md' },
      ],
    },
  },
}
