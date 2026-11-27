//! Links deunify. `DEUNIFY_LIB_DIR` names the directory holding `deunify_static.lib` (or
//! `deunify.lib` with `DEUNIFY_SHARED=1`); by default the CMake build directory of this
//! repository. The CMake build uses the static C runtime (`/MT`); a Rust crate linking the
//! static library needs `-C target-feature=+crt-static`, otherwise link the shared one.

use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=DEUNIFY_LIB_DIR");
    println!("cargo:rerun-if-env-changed=DEUNIFY_SHARED");
    let dir = std::env::var_os("DEUNIFY_LIB_DIR").map(PathBuf::from).unwrap_or_else(|| {
        PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("set by cargo")).join("../../../build")
    });
    println!("cargo:rustc-link-search=native={}", dir.display());
    let shared = std::env::var_os("DEUNIFY_SHARED").is_some_and(|v| v == "1");
    if shared {
        println!("cargo:rustc-link-lib=dylib=deunify");
        return;
    }
    println!("cargo:rustc-link-lib=static=deunify_static");
    println!("cargo:rustc-link-search=native={}", dir.join("_deps/zlib-build").display());
    println!("cargo:rustc-link-lib=static=zlibstatic");
    for system in ["d3d12", "dxgi", "onecore"] {
        println!("cargo:rustc-link-lib=dylib={system}");
    }
}
