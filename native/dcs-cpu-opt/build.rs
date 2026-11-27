//! Builds the vendored libdeflate 1.26 decompressor (MIT, `third_party/libdeflate`) with the dcs-cpu-opt
//! zlib-strict patch (`LIBDEFLATE_ZLIB_STRICT`), so `ZlibInflateOpt` rejects exactly what zlib-ng rejects.
//! Only the raw-DEFLATE decompressor is compiled (MSVC /O2; CRT linkage follows the crate's `crt-static`).
fn main() {
    let src = "third_party/libdeflate";
    cc::Build::new()
        .include(src)
        .file(format!("{src}/lib/deflate_decompress.c"))
        .file(format!("{src}/lib/utils.c"))
        .file(format!("{src}/lib/x86/cpu_features.c"))
        .define("LIBDEFLATE_ZLIB_STRICT", None)
        .opt_level(2)
        .warnings(false)
        .compile("libdeflate_strict");
    println!("cargo:rerun-if-changed={src}/lib");
    println!("cargo:rerun-if-changed={src}/libdeflate.h");
    println!("cargo:rerun-if-changed=build.rs");
}
