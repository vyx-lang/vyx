/*
 * 教程站的章节骨架。中英文文档在 docs/ 下成对存在，这里只声明配对关系与文案，
 * 正文由 scripts/build-tutorial.mjs 在构建时从 markdown 抽出（生成物不入库）。
 *
 * manifest 用于两侧小标题数量不一致的章节：显式说明哪些中文小节合并进同一个
 * 英文小节；不写 manifest 的章节按 h2 顺序一一对应。
 */
export const chapterOrder = ['basics', 'intermediate', 'advanced', 'migration']

export const chapters = {
  basics: {
    zh: '入门指南_ZH.md',
    en: 'TUTORIAL.md',
    label: { zh: '入门', en: 'Basics' },
    stage: { zh: '第一步', en: 'START HERE' },
    blurb: {
      zh: '安装 SDK、编译第一个程序，十节课走完变量、控制流、函数、结构体与类。',
      en: 'Install the SDK, compile the first program, and cover bindings, control flow, functions, structs, and classes in ten lessons.',
    },
  },
  intermediate: {
    zh: '进阶教程_ZH.md',
    en: 'INTERMEDIATE_TUTORIAL.md',
    label: { zh: '进阶', en: 'Intermediate' },
    stage: { zh: '第 11—20 课', en: 'LESSONS 11—20' },
    blurb: {
      zh: '集合、闭包、泛型、match、错误处理与多文件项目，直接对上标准库与构建系统。',
      en: 'Collections, closures, generics, match, error handling, and multi-file projects meet the standard library and the build system.',
    },
  },
  advanced: {
    zh: '高级特性_ZH.md',
    en: 'ADVANCED_FEATURES.md',
    label: { zh: '高级特性', en: 'Advanced' },
    stage: { zh: '第 21—43 课', en: 'LESSONS 21—43' },
    blurb: {
      zh: '资源管理、异步、trait、所有权与借用、comptime、继承、运行时反射与跨语言互操作。',
      en: 'Resource management, async, traits, ownership and borrowing, comptime, inheritance, runtime reflection, and interop.',
    },
  },
  migration: {
    zh: '快速迁移_Rust_CPP.md',
    en: 'MIGRATING_FROM_RUST_CPP.md',
    label: { zh: '迁移对照', en: 'Coming from Rust or C++' },
    stage: { zh: '按图索骥', en: 'SIDE BY SIDE' },
    blurb: {
      zh: '把 Rust 与 C++ 的写法逐条对照到 Vyx：语法、控制流、错误处理、内存与构建。',
      en: 'Map Rust and C++ habits onto Vyx, line by line: syntax, control flow, errors, memory, and builds.',
    },
    // 英文版把几条相邻主题合并成一节，这里显式说清楚。
    manifest: [
      { zh: ['基础语法'], en: ['Basic syntax'], id: 'basic-syntax' },
      { zh: ['控制流'], en: ['Control flow'], id: 'control-flow' },
      { zh: ['结构体和方法', '类和 OOP'], en: ['Structs, methods, and classes'], id: 'structs-and-classes' },
      { zh: ['错误处理'], en: ['Error handling'], id: 'error-handling' },
      { zh: ['内存管理'], en: ['Ownership and resource management'], id: 'memory-management' },
      { zh: ['泛型', '容器'], en: ['Generics and containers'], id: 'generics-and-containers' },
      { zh: ['并发'], en: ['Concurrency'], id: 'concurrency' },
      { zh: ['C 互操作', '构建系统'], en: ['C interoperability and builds'], id: 'c-interop-and-builds' },
      { zh: ['一句话总结'], en: ['In one line'], id: 'wrap-up' },
    ],
  },
}

/*
 * 分节图示：每个图示都由页面时钟（--motion-progress）驱动，暂停或减少动效时静止。
 * 键是章节 id，值是章节开头的图示与若干节内图示（按节的英文标题片段匹配）。
 * 节内匹配用的是「分节 id 是否包含该片段」，所以片段必须真的出现在 id 里；
 * 早先写的 'collections' 匹配不到 s01-lesson-11-collection-containers，已改为 'collection'。
 */
export const chapterFigures = {
  basics: { hero: 'pipeline', sections: [['loops', 'loop']] },
  intermediate: { hero: 'result', sections: [['collection', 'containers']] },
  advanced: { hero: 'ownership', sections: [['async', 'result']] },
  migration: { hero: 'languages', sections: [] },
}
