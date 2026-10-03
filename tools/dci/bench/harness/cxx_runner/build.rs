// Build script for the cxx binding path.  Compiles the #[cxx::bridge] glue plus
// the shared C++ fixture library with the selected C++ compiler.
fn main() {
    let mut build = cxx_build::bridge("src/main.rs");
    build
        .file("src/abi_fixtures.cpp")
        .std("c++17")
        .include("src");
    #[cfg(target_env = "msvc")]
    build.flag("/EHsc");
    if let Ok(extra) = std::env::var("BENCH_CXX_FLAGS") {
        for flag in extra.split_whitespace() {
            build.flag_if_supported(flag);
        }
    }
    build.compile("cxx_runner_bridge");
    println!("cargo:rerun-if-changed=src/main.rs");
    println!("cargo:rerun-if-changed=src/bridge.h");
    println!("cargo:rerun-if-changed=src/abi_fixtures.cpp");
    println!("cargo:rerun-if-changed=src/abi_fixtures.hpp");
    println!("cargo:rerun-if-env-changed=BENCH_CXX_FLAGS");
}
