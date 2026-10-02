// Reading examples. Their full project setup is documented in docs/MOSP.md.
import { dciCases } from './dci-examples.js'

export const features = [
  {
    id: 'migrate', label: 'Migrate',
    title: { zh: '模块版本与迁移', en: 'Module versions and migration' },
    description: { zh: '保留两版运费规则，用注解记录版本与签名的变化。普通调用使用选定版本，显式版本调用使用历史实现。', en: 'Keep two shipping policies and annotate their versions and changed signatures. A normal call uses the selected version; an explicit version call uses the historical implementation.' },
    footnote: { zh: '项目需提供对应版本的模块；默认版本由锁文件选择。', en: 'The project provides both module versions; the lockfile selects the default.' },
    output: 'quote(3) → 11   ·   quote@1.0.0["standard"](3) → 14',
    files: [
      { name: 'shipping_v1.vyx', language: 'Vyx', code: `@[version("1.0.0")]
@[variant("standard")]
module Shipping;

// Base delivery fee plus a per-kilogram rate.
public fn quote(weight_kg: i32) -> i32 {
    return 8 + weight_kg * 2;
}` },
      { name: 'shipping_v2.vyx', language: 'Vyx', code: `@[version("2.0.0")]
@[variant("standard")]
module Shipping;

@[migrate(
    fromVer = "1.0.0",
    fromSig = fn quote(i32) -> i32,
    desc = "Lower the base delivery fee"
)]
public fn quote(weight_kg: i32) -> i32 {
    return 5 + weight_kg * 2;
}` },
      { name: 'main.vyx', language: 'Vyx', code: `use Shipping;

fn main() -> i32 {
    let current = quote(3);
    let previous = quote@1.0.0["standard"](3);

    print(current);     // 11
    print(previous);    // 14
    return 0;
}` },
    ],
  },
  {
    id: 'reflection', label: 'Reflection',
    title: { zh: '类型发现与方法绑定', en: 'Type discovery and method binding' },
    description: { zh: '用 @[reflect] 登记类型和方法，通过 std.reflect 按名称查找。这个例子找到一个命令，创建实例，再绑定并调用 run 方法。', en: 'Register types and methods with @[reflect], then look them up through std.reflect. This example finds a command, creates an instance, and binds and calls its run method.' },
    footnote: { zh: '@[hidden] 排除成员；运行时反射会保留相应元数据和可调用代码。', en: '@[hidden] excludes members. Runtime reflection retains the registered metadata and callable code.' },
    output: 'WelcomeCommand → run → fn() -> void   /   Welcome to Vyx!',
    files: [
      { name: 'commands.vyx', language: 'Vyx', code: `module commands;

@[reflect("WelcomeCommand")]
public class WelcomeCommand {
    private runs: i32;

    public WelcomeCommand() {
        self.runs = 0;
    }

    @[reflect(alias = "run")]
    public fn execute(self) -> void {
        self.runs = self.runs + 1;
        print("Welcome to Vyx!");
    }

    @[hidden]
    public fn reset(self) -> void {
        self.runs = 0;
    }
}` },
      { name: 'main.vyx', language: 'Vyx', code: `use std.reflect;
use commands;

fn main() -> i32 {
    let command_type = getType("WelcomeCommand");
    var instance = command_type.create();
    let method = instance.getMethod("run");
    let run = method.as::<fn() -> void>();

    run();  // Welcome to Vyx!
    return 0;
}` },
    ],
  },
  {
    id: 'dci', label: 'DCI',
    title: { zh: '导入原生类型与泛型', en: 'Import native types and generics' },
    description: { zh: '导入 C++ 类，覆写方法供原生代码回调，或调用 Rust / C++ 泛型。切换文件查看原始定义、DCI 声明和 Vyx 调用。', en: 'Import a C++ class, override a method for native callbacks, or call Rust / C++ generics. Switch files to see the original definitions, DCI declarations and Vyx calls.' },
    footnote: { zh: '.dcib 提供类型布局、调用 ABI 与生命周期；完整项目包含契约生成和链接配置。', en: '.dcib supplies layouts, calling ABIs and lifecycles. The complete projects include contract generation and linking configuration.' },
    cases: dciCases,
  },
  {
    id: 'dce', label: 'DCE',
    title: { zh: '可达性分析与代码裁剪', en: 'Reachability and code elimination' },
    description: { zh: '编译器从入口、导出和反射登记追踪需要的代码，删除未使用的函数。析构、外部调用和其他副作用也参与分析。', en: 'The compiler follows the code needed by entries, exports, and reflection registrations, removing unused functions. Destruction, native calls, and other effects also participate in the analysis.' },
    footnote: { zh: '编译流程示意，数字不代表程序大小。', en: 'An illustration of compiler flow; counts do not measure program size.' },
    files: [],
  },
]
