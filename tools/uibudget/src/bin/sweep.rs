//! Where the layout's contiguous-block cliffs are, as a function of node count.
//!
//! Builds N absolutely-positioned rectangles under the root, relayouts, draws,
//! and reports the largest single block the allocator was asked for. That is
//! the number pocketjs_idf_rust_alloc either finds or answers NULL to.
//!
//!     cargo run --release --bin sweep
//!     POCKETUI_CAP=40000 cargo run --release --bin sweep
//!
//! POCKETUI_CAP only decides the `fits` column here; the allocator is left
//! uncapped so the whole table can be printed. `screen` is the binary that
//! actually refuses blocks and reproduces the abort.

use uibudget::*;

#[global_allocator]
static A: Track = Track;

fn main() {
    // Read the ceiling, then put the allocator back: a capped sweep would abort
    // on the first row that does not fit and print no table at all.
    let cap = cap_from_env();
    set_cap(usize::MAX);
    println!("largest free block assumed: {cap} bytes");
    println!("{:>6} {:>6} {:>16} {:>8}", "nodes", "taffy", "biggest block", "fits");

    let mut previous = usize::MAX;
    for n in 1..=40u32 {
        let mut ui = board(&[]);
        for i in 0..n {
            let id = ui.create_node(TYPE_VIEW);
            ui.set_prop(id, P_POS_TYPE, 1.0);
            ui.set_prop(id, P_INSET_L, (i % 20) as f64 * 11.0);
            ui.set_prop(id, P_INSET_T, (i / 20) as f64 * 11.0);
            ui.set_prop(id, P_WIDTH, 8.0);
            ui.set_prop(id, P_HEIGHT, 8.0);
            ui.set_prop(id, P_BG_COLOR, 0xff00_00ffu32 as f64);
            ui.insert_before(ROOT, id, 0);
        }
        let mut strip = Strip::new();
        // Only the relayout and the draw are measured; the tree and the strip
        // buffer were built above, as they are on the device.
        arm();
        ui.tick();
        let words: Vec<u32> = ui.draw().words.clone();
        let _ = render_frame(&ui, &words, &mut strip);
        disarm();

        let block = biggest();
        let fits = block <= cap;
        // Only the steps are interesting; the flat stretches between them are
        // noise, and printing 40 near-identical rows hides the shape.
        if block != previous || n == 1 {
            println!(
                "{:>6} {:>6} {:>16} {:>8}",
                n,
                n + 1,
                block,
                if fits { "yes" } else { "NO" }
            );
            previous = block;
        }
        // What pocket_ui.c predicts must match what actually happened, or the
        // C-side budget is guarding the wrong number.
        let predicted = layout_block(n);
        if predicted > 0 && block < predicted {
            println!("  !! layout_block({n}) says {predicted} but the run asked for {block}");
        }
    }
    println!(
        "\nmain/pocket_ui.c encodes these steps as layout_block(). safeNodes is\n\
         the last node count before the first step; maxNodes is the last count\n\
         still inside the first step."
    );
}
