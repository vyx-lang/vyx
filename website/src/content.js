export const repository = "https://github.com/vyx-lang/vyx"
export const releases = repository + '/releases/latest'
export const downloads = [
  {
    "label": "Windows x86_64",
    "href": "https://github.com/vyx-lang/vyx/releases/latest/download/vyx-sdk-windows-x86_64-llvm22.zip"
  },
  {
    "label": "Linux x86_64",
    "href": "https://github.com/vyx-lang/vyx/releases/latest/download/vyx-sdk-linux-x86_64-llvm22.tar.gz"
  }
]

export const messages = {
  "en": {
    "skip": "Skip to content",
    "nav": [
      "The language",
      "Interoperability",
      "Get started"
    ],
    "docs": "Installation guide",
    "language": "切换为中文",
    "theme": "Switch color theme",
    "close": "Close",
    "menu": "Open menu",
    "interopCta": "Explore MOSP and DCI",
    "copied": "Copied",
    "commandCopy": "Copy command",
    "footerStatus": "Early access",
    "copyFail": "Select and copy the text manually.",
    "modalTitle": "Install and run",
    "dciTitle": "Native interoperability with DCI",
    "modalIntro": "Download the SDK, save a source file, and compile it with vyxc.",
    "dciIntro": "Import native types and functions with a contract generated from their compiler.",
    "steps": [
      {
        "title": "Install the SDK",
        "body": "Download the SDK for your system, extract it, and add its bin directory to PATH. Windows and Linux SDKs include the LLVM backend. Keep the extracted directory structure intact, then run vyxc --version to check the installation."
      },
      {
        "title": "Create hello.vyx",
        "body": "Save the example below in a new file."
      },
      {
        "title": "Compile and run",
        "body": "Run this command from the same directory."
      }
    ],
    "dciSections": [
      {
        "title": "Use the original compiler",
        "body": "The C++ or Rust compiler determines the layout, calling convention, and construction and destruction of the imported types."
      },
      {
        "title": "Generate the contract",
        "body": "An adapter writes those details to a .dcib contract. For supported generics, it requests concrete instances from the original compiler during the build and supplies their compiled artifacts."
      },
      {
        "title": "Build the Vyx caller",
        "body": "Vyx checks the contract, generates native calls, and links libraries and required bridges. Inheritance, virtual callbacks, and generics use their declared ABIs and lifecycles."
      }
    ],
    "repoLink": "Read the full tutorial"
  },
  "zh": {
    "skip": "跳至正文",
    "nav": [
      "语言特性",
      "跨语言互操作",
      "开始使用"
    ],
    "docs": "安装指南",
    "language": "Switch to English",
    "theme": "切换明暗主题",
    "close": "关闭",
    "menu": "打开菜单",
    "interopCta": "了解 MOSP 与 DCI",
    "copied": "已复制",
    "commandCopy": "复制命令",
    "footerStatus": "开发中 · Early access",
    "copyFail": "请选中文本手动复制。",
    "modalTitle": "安装与运行",
    "dciTitle": "DCI 原生互操作",
    "modalIntro": "下载 SDK，保存源文件，然后用 vyxc 编译运行。",
    "dciIntro": "从原语言编译器生成契约，在 Vyx 中导入原生类型与函数。",
    "steps": [
      {
        "title": "安装 SDK",
        "body": "下载对应系统的 SDK，解压并将 bin 目录加入 PATH。Windows 与 Linux SDK 均已内置 LLVM 后端。保留解压后的目录结构，再执行 vyxc --version 确认安装。"
      },
      {
        "title": "创建 hello.vyx",
        "body": "将下面的代码保存到新文件中。"
      },
      {
        "title": "编译并运行",
        "body": "在文件所在目录执行下方命令。"
      }
    ],
    "dciSections": [
      {
        "title": "使用原语言编译器",
        "body": "导入类型的布局、调用约定、构造与析构，由对应的 C++ 或 Rust 编译器确定。"
      },
      {
        "title": "生成契约",
        "body": "适配器将这些信息写入 .dcib 契约。对受支持的泛型，构建时向原语言编译器请求具体实例，并提供编译产物。"
      },
      {
        "title": "构建 Vyx 调用方",
        "body": "Vyx 检查契约，生成原生调用，再链接库和所需桥接。继承、虚方法回调与泛型使用各自声明的 ABI 和生命周期。"
      }
    ],
    "repoLink": "阅读完整教程"
  }
}
