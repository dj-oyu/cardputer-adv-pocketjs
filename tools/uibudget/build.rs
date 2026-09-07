// The latin atlases come from tools/make_font.py at build time rather than
// from committed binaries, so the harness always measures the fonts the
// firmware loads.
fn main() {
    let out = std::env::var("OUT_DIR").unwrap();
    let root = std::env::var("CARGO_MANIFEST_DIR").unwrap();
    println!("cargo:rerun-if-changed=dump_atlas.py");
    println!("cargo:rerun-if-changed=../make_font.py");
    let status = std::process::Command::new("python3")
        .arg(format!("{root}/dump_atlas.py"))
        .arg(&out)
        .status()
        .expect("python3 is needed to generate the font atlases");
    assert!(status.success(), "dump_atlas.py failed");
}
