#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/.cache/tools/export-esp.sh"
export PATH="$HOME/.cargo/bin:$PATH"
RUST_BIN="$(dirname "$(rustup which --toolchain pocketjs-esp rustc)")"
export PATH="$RUST_BIN:$PATH"
export RUSTC="$RUST_BIN/rustc"
mkdir -p "$ROOT/.cache/native"
"$RUSTC" --version
"$RUSTC" --edition=2021 "$ROOT/.cache/pocketjs/hosts/esp-idf/components/pocketjs_ui_core/rustc_wrapper.rs" -o "$ROOT/.cache/native/rustc-wrapper"
export RUSTC_WRAPPER="$ROOT/.cache/native/rustc-wrapper"
export RUSTFLAGS="" CARGO_ENCODED_RUSTFLAGS=""
for component in ui-core render-rgb565; do
  export POCKETJS_RUST_NAMESPACE="pocketjs_idf_${component//-/_}"
  export CARGO_TARGET_DIR="$ROOT/.cache/cargo/$component"
  cargo build -Zbuild-std=core,alloc --release --locked --no-default-features --target xtensa-esp32s3-none-elf --manifest-path "$ROOT/.cache/pocketjs/hosts/esp-idf/native/$component/Cargo.toml"
  cp "$CARGO_TARGET_DIR/xtensa-esp32s3-none-elf/release/lib${POCKETJS_RUST_NAMESPACE}.a" "$ROOT/.cache/native/"
done
