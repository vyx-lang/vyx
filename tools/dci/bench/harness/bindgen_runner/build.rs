// Build script for the extern "C" / bindgen path.  Compiles the shared C++
// fixture library plus its hand-authored C facade, then generates Rust bindings
// from the C header with bindgen.
use std::{env, path::PathBuf};

fn main() {
    let mut build = cc::Build::new();
    build
        .cpp(true)
        .std("c++17")
        .file("src/abi_fixtures.cpp")
        .file("src/abi_fixtures_c.cpp")
        .include("src");
    #[cfg(target_env = "msvc")]
    build.flag("/EHsc");
    if let Ok(extra) = std::env::var("BENCH_CXX_FLAGS") {
        for flag in extra.split_whitespace() {
            build.flag_if_supported(flag);
        }
    }
    build.compile("abi_fixtures_c");

    // The fixtures use new/delete and throw std::runtime_error, so the C++
    // runtime must be linked.  On MSVC, cc::Build already pulls in the C++ CRT.
    #[cfg(not(target_env = "msvc"))]
    println!("cargo:rustc-link-lib=stdc++");

    let bindings = bindgen::Builder::default()
        .header("src/abi_fixtures_c.h")
        .clang_arg("-Isrc")
        .allowlist_function("abi_c_.*")
        .allowlist_type("Abi.*")
        .generate()
        .expect("bindgen failed to generate bindings");

    let out = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out.join("bindings.rs"))
        .expect("write bindings.rs");

    println!("cargo:rerun-if-changed=src/abi_fixtures_c.h");
    println!("cargo:rerun-if-changed=src/abi_fixtures_c.cpp");
    println!("cargo:rerun-if-changed=src/abi_fixtures.cpp");
    println!("cargo:rerun-if-changed=src/abi_fixtures.hpp");
    println!("cargo:rerun-if-env-changed=BENCH_CXX_FLAGS");
}
