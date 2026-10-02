const vectorCode = `@[dci_import("vector.dcib")]
extern "dci" {
    @[dci_list_init(true)]
    class std.vector<T> {
        vector();
        ~vector();
        public fn push_back(value: &T);
        public fn size() const -> u64;
        public fn capacity() const -> u64;
        public fn reserve(n: u64);
        public fn clear();
        public fn pop_back();
        public fn at(n: u64) -> &mut T;
        public fn data() -> *T;
    };
}

fn main() -> i32 {
    var a = std.vector<i32>{1, 2, 3, 4, 5};
    let value: i32 = 6;
    a.push_back(&value);

    var b = std.vector<f64>{0.5, 1.5, 2.5};
    let fraction: f64 = 3.5;
    b.push_back(&fraction);

    print(a.size());
    print(b.size());
    return 0;
}`

export const mospContent = {
  zh: {
    label: 'MOSP',
    title: '版本、类型、原生接口',
    intro: '模块可以保留历史版本，类型可以按名称查询，外部库通过 ABI 契约接入。MOSP（Metadata-Oriented Systems Programming）将这些信息提供给编译器，用于选择实现、生成调用和裁剪代码。',
    overviewTitle: 'MOSP 的四项功能',
    overviewIntro: '版本、类型和接口声明贯穿编译与运行。Migrate、Reflection、DCI 和 DCE 分别使用这些信息。',
    features: [
      { id: 'migrate', name: 'Migrate', description: '关联模块的当前与历史声明，按版本选择实现。' },
      { id: 'reflection', name: 'Reflection', description: '登记可发现的类型和成员，查询、绑定并调用。' },
      { id: 'dci', name: 'DCI', description: '描述外部实体的 ABI，让 Vyx 使用原生类型和实现。' },
      { id: 'dce', name: 'DCE', description: '追踪入口、导出和反射所需的代码，移除无用部分。' },
    ],
    dciTitle: 'DCI：使用原生类型与实现',
    dciIntro: 'DCI（Declarative Code Interface）将外部类型的布局、调用与生命周期写成契约。C++ 模板和 Rust 泛型仍由原编译器处理；适配器生成 .dcib，Vyx 根据契约生成调用并链接原生实现。',
    pipelineTitle: '从声明到原生调用',
    pipelineIntro: '调用方选择泛型的类型实参，Active Adapter 在构建时向原语言编译器请求具体实例。',
    pipeline: [
      { id: 'request', name: '声明与请求', detail: 'Vyx 声明外部 API；调用点确定具体类型实参。' },
      { id: 'producer', name: '原工具链编译', detail: '检查泛型约束，确定布局和调用 ABI，生成所需实现。' },
      { id: 'contract', name: '契约验证', detail: '.dcib 记录具体实例；Vyx 检查目标、符号与生命周期要求。' },
      { id: 'link', name: '生成与链接', detail: '生成直接调用或所需桥接，与原生对象文件和库一起链接。' },
    ],
    vectorTitle: '原始 std::vector<T>',
    vectorIntro: '头文件只包含 <vector>。Vyx 声明开放的 std.vector<T>，在调用处选择 i32 或 f64，再用 C++ 的花括号初始化规则构造容器。',
    vectorNote: '完整项目包含 vector.dcib 和 Active Adapter 配置。构建时实例化调用方选择的类型；构造、成员调用和析构使用匹配的 C++ 实现。',
    vectorCode,
    factsTitle: '表示、调用与生命周期',
    facts: [
      { name: '表示', detail: '对象多大、如何对齐、字段和基类位于哪里。' },
      { name: '调用', detail: '调用哪个符号，参数和返回值如何通过目标 ABI 传递。' },
      { name: '生命周期', detail: '对象如何构造、复制、移动与析构，存储由谁释放。' },
      { name: '有效范围', detail: '事实属于哪个目标、工具链和构建配置；实现产物必须与契约匹配。' },
    ],
    ecosystemTitle: '原生库与调用方式',
    ecosystemIntro: '容器、文本和校验和沿用各自的原生实现。完整工程包含接口声明与构建配置。',
    ecosystem: [
      { id: 'vector', name: 'std::vector<T>', language: 'C++', description: '可变长数组：构造、追加、借用访问和容量管理。', detail: '原始 std::vector<T>；Windows x86_64 MSVC。', path: 'probes/gates/dci-vector/README.md' },
      { id: 'icu', name: 'ICU 78.3', language: 'C++', description: '处理 Unicode 文本：UTF-16、码点迭代、复制与大小写转换。', detail: '链接 UnicodeString 和 ICU 数据；Windows x86_64 MSVC。', path: 'probes/gates/dci-cpp-ecosystem/README.md' },
      { id: 'cargo', name: 'crc32fast / adler2', language: 'Rust / Cargo', description: '计算 CRC-32 与 Adler-32 校验和，沿用 Cargo 的依赖和 feature 配置。', detail: 'Rust native bridge 传递 slice；Windows x86_64 MSVC。', path: 'probes/gates/dci-rust-ecosystem/README.md' },
      { id: 'unwind', name: 'shared_abi 异常', language: 'C++ / Vyx', description: 'C++ 异常穿过 Vyx 调用链，退出作用域时清理对象。', detail: 'Windows MSVC / Linux Itanium，x86_64。', path: 'probes/gates/dci-exceptions/README.md' },
      { id: 'multilang', name: '多语言同一工程', language: 'C++ / Rust / Zig', description: '结合 C++ 类、Rust trait 回调与 Zig 按值记录。', detail: '各语言保留自己的契约与原生实现；Windows x86_64 MSVC。', path: 'docs/DCI_SPEC_ZH.md' },
    ],
    failureTitle: '异常传播与对象清理',
    failureIntro: 'AOT shared_abi 保留 C++ 异常身份。栈展开时按逆构造顺序析构已完成的对象，并释放由 Vyx 管理的 DCI 存储。',
    failurePoints: [
      '构造失败只释放已分配存储；栈内对象执行析构，不执行堆释放。',
      '调用方与被调用操作声明同一版本的异常 ABI，由契约确定传播路径。',
      'Rust panic、Zig error 须通过各自受支持的返回或翻译路径处理。',
    ],
    scopeTitle: '接入配置',
    scopeIntro: '契约与库属于具体目标、工具链和配置。更换编译器或依赖时，同步生成匹配的契约与实现。',
    scope: [
      { name: '执行方式', value: 'LLVM AOT；JIT 对齐是后续工作。' },
      { name: '示例目标', value: 'Windows x86_64 MSVC；共享异常另有 Linux x86_64 工程。' },
      { name: '调用方式', value: 'Direct 使用原生符号；Stub / native bridge 提供所需桥接。' },
      { name: '接入条件', value: '按操作检查布局、调用和生命周期；支持的签名由 Adapter 与目标 ABI 确定。' },
    ],
    readingTitle: '配置、规范与完整示例',
    links: [
      { title: 'MOSP 核心特性', description: '版本迁移、反射、DCI 与 DCE 的具体用法。', path: 'docs/MOSP_ZH.md' },
      { title: 'DCI 规范', description: '契约内容、验证规则与当前能力矩阵。', path: 'docs/DCI_SPEC_ZH.md' },
      { title: 'Adapter 工具指南', description: '契约生成、生产端配置和 bridge 边界。', path: 'tools/dci/README.zh-CN.md' },
      { title: '开放泛型项目', description: 'C++ / Rust 实例请求和完整构建配置。', path: 'tests/projects/dci_opengeneric/README.md' },
    ],
  },
  en: {
    label: 'MOSP',
    title: 'Versions. Types. Native code.',
    intro: 'Keep historical module versions, look up types by name, and connect native libraries through ABI contracts. MOSP (Metadata-Oriented Systems Programming) gives the compiler this information to select implementations, generate calls and remove unused code.',
    overviewTitle: 'Four features of MOSP',
    overviewIntro: 'Version, type and interface declarations are used during compilation and at runtime. Migrate, Reflection, DCI and DCE each use that information.',
    features: [
      { id: 'migrate', name: 'Migrate', description: 'Connect current and historical module declarations and select implementations by version.' },
      { id: 'reflection', name: 'Reflection', description: 'Register discoverable types and members, then query, bind and call them.' },
      { id: 'dci', name: 'DCI', description: 'Describe foreign ABI entities so Vyx can use native types and implementations.' },
      { id: 'dce', name: 'DCE', description: 'Follow the code needed by entries, exports and reflection, and remove unused parts.' },
    ],
    dciTitle: 'DCI: use native types and implementations',
    dciIntro: 'DCI (Declarative Code Interface) records foreign layouts, calls and lifecycles in a contract. The original compiler still handles C++ templates and Rust generics. An adapter generates .dcib; Vyx uses it to generate calls and link native implementations.',
    pipelineTitle: 'From declarations to native calls',
    pipelineIntro: 'The caller chooses generic type arguments. An Active Adapter requests concrete instances from the original compiler during the build.',
    pipeline: [
      { id: 'request', name: 'Declare and request', detail: 'Vyx declares the external API. Call sites determine concrete type arguments.' },
      { id: 'producer', name: 'Compile with the original toolchain', detail: 'Check generic constraints, determine layouts and calling ABIs, and generate implementations.' },
      { id: 'contract', name: 'Validate the contract', detail: '.dcib records concrete instances. Vyx checks targets, symbols and lifecycle requirements.' },
      { id: 'link', name: 'Generate and link', detail: 'Emit direct calls or the required bridges, then link native object files and libraries.' },
    ],
    vectorTitle: 'The original std::vector<T>',
    vectorIntro: 'The header only includes <vector>. Declare open std.vector<T> in Vyx, choose i32 or f64 at the call site, and construct the container using C++ brace initialization rules.',
    vectorNote: 'The complete project includes vector.dcib and Active Adapter configuration. The build instantiates the types chosen by the caller. Construction, member calls and destruction use the matching C++ implementation.',
    vectorCode,
    factsTitle: 'Representation, calls, and lifecycles',
    facts: [
      { name: 'Representation', detail: 'Object size and alignment, field locations and base offsets.' },
      { name: 'Calling', detail: 'The exact symbol and how the target ABI passes parameters and returns.' },
      { name: 'Lifecycle', detail: 'How objects are constructed, copied, moved and destroyed, and who releases their storage.' },
      { name: 'Validity', detail: 'The target, toolchain and build configuration the facts belong to. Implementation artifacts must match the contract.' },
    ],
    ecosystemTitle: 'Native libraries and call paths',
    ecosystemIntro: 'Containers, text and checksums use their native implementations. The complete projects include interface declarations and build configuration.',
    ecosystem: [
      { id: 'vector', name: 'std::vector<T>', language: 'C++', description: 'Growable arrays: construction, append, borrowed access and capacity management.', detail: 'Original std::vector<T>; Windows x86_64 MSVC.', path: 'probes/gates/dci-vector/README.md' },
      { id: 'icu', name: 'ICU 78.3', language: 'C++', description: 'Unicode text: UTF-16, code-point iteration, copying and case conversion.', detail: 'Links UnicodeString and ICU data; Windows x86_64 MSVC.', path: 'probes/gates/dci-cpp-ecosystem/README.md' },
      { id: 'cargo', name: 'crc32fast / adler2', language: 'Rust / Cargo', description: 'Compute CRC-32 and Adler-32 checksums with Cargo dependencies and feature configuration.', detail: 'Rust native bridges pass slices; Windows x86_64 MSVC.', path: 'probes/gates/dci-rust-ecosystem/README.md' },
      { id: 'unwind', name: 'shared_abi exceptions', language: 'C++ / Vyx', description: 'C++ exceptions pass through Vyx calls and clean up objects as scopes exit.', detail: 'Windows MSVC / Linux Itanium, x86_64.', path: 'probes/gates/dci-exceptions/README.md' },
      { id: 'multilang', name: 'One multilingual project', language: 'C++ / Rust / Zig', description: 'Combine C++ classes, Rust trait callbacks and Zig records passed by value.', detail: 'Each language keeps its contracts and native implementation; Windows x86_64 MSVC.', path: 'docs/DCI_SPEC.md' },
    ],
    failureTitle: 'Exception propagation and object cleanup',
    failureIntro: 'AOT shared_abi preserves C++ exception identity. Unwinding destroys fully constructed objects in reverse construction order and releases DCI storage managed by Vyx.',
    failurePoints: [
      'Failed construction releases allocated storage only. Inline stack objects receive destruction without a heap release.',
      'The caller and called operation declare the same versioned exception ABI. The contract determines the propagation path.',
      'Rust panic and Zig errors require their own supported return or translation paths.',
    ],
    scopeTitle: 'Integration configuration',
    scopeIntro: 'Contracts and libraries belong to a specific target, toolchain and configuration. Regenerate matching contracts and implementations when compilers or dependencies change.',
    scope: [
      { name: 'Execution', value: 'LLVM AOT. JIT parity is later work.' },
      { name: 'Example targets', value: 'Windows x86_64 MSVC, with a separate Linux x86_64 shared exception project.' },
      { name: 'Call path', value: 'Direct uses native symbols. Stubs and native bridges supply the required adapters.' },
      { name: 'Integration requirements', value: 'Check layout, calls and lifecycles per operation. The Adapter and target ABI determine the supported signatures.' },
    ],
    readingTitle: 'Configuration, specifications and complete examples',
    links: [
      { title: 'MOSP core features', description: 'Practical uses of migration, reflection, DCI and DCE.', path: 'docs/MOSP.md' },
      { title: 'DCI specification', description: 'Contract contents, validation rules and the current capability matrix.', path: 'docs/DCI_SPEC.md' },
      { title: 'Adapter tools guide', description: 'Contract generation, producer configuration and bridge boundaries.', path: 'tools/dci/README.md' },
      { title: 'Open generics project', description: 'C++ / Rust instance requests and complete build configuration.', path: 'tests/projects/dci_opengeneric/README.md' },
    ],
  },
}
