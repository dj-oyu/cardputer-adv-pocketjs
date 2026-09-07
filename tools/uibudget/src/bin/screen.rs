//! Replays the node tree main/pocket_ui.c builds for apps/pocketui/pocketui.js,
//! one element at a time, under an allocator that refuses blocks the device
//! could not have found. The first step that aborts is the smallest thing that
//! would take the board down.
//!
//!     cargo run --release --bin screen            # the device's 23552 ceiling
//!     POCKETUI_CAP=none cargo run --release --bin screen
//!     cargo run --release --bin screen 5          # just one step
//!     cargo run --release --bin screen 9 1        # one node past the cliff
//!
//! To try a screen of your own, edit build(): it is a straight list of the
//! create/set_prop/set_text/insert calls the surface makes, in the order it
//! makes them, and nothing here is clever about which ones you use.

use pocketjs_core::Ui;
use uibudget::*;

#[global_allocator]
static A: Track = Track;

/// The steps, as a superset ladder: each includes everything above it.
const STEPS: [&str; 10] = [
    "a bare screen",
    "+ one large text",
    "+ a rect",
    "+ one small text",
    "+ one body text (the dynamic Japanese slot)",
    "+ the rest of the labels",
    "+ a list",
    "+ a text created and removed",
    "+ a second screen pushed and popped",
    "+ a toast (everything pocketui.js builds)",
];

struct Host {
    ui: Ui,
    nodes: u32,
}

impl Host {
    fn create(&mut self, t: u8) -> i32 {
        self.nodes += 1;
        self.ui.create_node(t)
    }
    /// core_box() from pocket_ui.c: absolute, clipped, insets counted from the
    /// containing block.
    fn boxx(&mut self, id: i32, x: f64, y: f64, w: f64, h: f64) {
        self.ui.set_prop(id, P_POS_TYPE, 1.0);
        self.ui.set_prop(id, P_INSET_L, x);
        self.ui.set_prop(id, P_INSET_T, y);
        self.ui.set_prop(id, P_WIDTH, w);
        self.ui.set_prop(id, P_HEIGHT, h);
        self.ui.set_prop(id, P_OVERFLOW, 1.0);
    }
    /// js_screen_text(): one node, in the order the C sets it up.
    fn text(&mut self, parent: i32, r: [f64; 4], colour: f64, slot: f64, s: &str) -> i32 {
        let id = self.create(TYPE_TEXT);
        self.boxx(id, r[0], r[1], r[2], r[3]);
        self.ui.set_prop(id, P_TEXT_COLOR, colour);
        self.ui.set_prop(id, P_FONT_SLOT, slot);
        self.ui.set_text(id, s);
        self.ui.insert_before(parent, id, 0);
        id
    }
}

fn build(step: usize, pad: u32) -> (Ui, u32) {
    // Both strings pocketui.js shows in font body.
    let mut h = Host {
        ui: board(&['日', '本', '語', 'も', '出', 'る', 'よ', 'う', 'こ', 'そ']),
        nodes: 0,
    };

    // pocket.ui.screen({background}) — hidden until push.
    let home = h.create(TYPE_VIEW);
    h.boxx(home, 0.0, 0.0, LCD_W as f64, LCD_H as f64);
    h.ui.set_prop(home, P_BG_COLOR, 0x0714_25ff as f64);
    if step == 0 {
        h.ui.insert_before(ROOT, home, 0);
        return (h.ui, h.nodes);
    }

    let title = h.text(home, [12.0, 22.0, 216.0, 18.0], 0xf0f8_ffffu32 as f64, 1.0, "Hello, World!");
    let _ = title;
    if step == 1 {
        h.ui.insert_before(ROOT, home, 0);
        return (h.ui, h.nodes);
    }

    let rect = h.create(TYPE_VIEW);
    h.boxx(rect, 12.0, 44.0, 216.0, 34.0);
    h.ui.set_prop(rect, P_BG_COLOR, 0x1233_4affu32 as f64);
    h.ui.set_prop(rect, P_RADIUS, 5.0);
    h.ui.insert_before(home, rect, 0);
    if step == 2 {
        h.ui.insert_before(ROOT, home, 0);
        return (h.ui, h.nodes);
    }

    h.text(home, [14.0, 8.0, 212.0, 12.0], 0x69cd_eeffu32 as f64, 0.0, "POCKET.UI");
    if step == 3 {
        h.ui.insert_before(ROOT, home, 0);
        return (h.ui, h.nodes);
    }
    h.text(home, [14.0, 50.0, 212.0, 12.0], 0x8ef0_c4ffu32 as f64, 2.0, "日本語も出る");
    if step == 4 {
        h.ui.insert_before(ROOT, home, 0);
        return (h.ui, h.nodes);
    }
    h.text(home, [14.0, 64.0, 212.0, 12.0], 0xa9ba_caffu32 as f64, 0.0, "ENTER +0");
    h.text(home, [14.0, 120.0, 212.0, 12.0], 0x8fa6_bcffu32 as f64, 0.0, "ESC QUITS");
    if step == 5 {
        h.ui.insert_before(ROOT, home, 0);
        return (h.ui, h.nodes);
    }

    // js_screen_list(): a container, one highlight that moves, and a label and
    // a detail per VISIBLE row. Rows are re-texted rather than added, so the
    // node cost follows the height and not the item count.
    let (lx, ly, lw, lh) = (12.0f64, 82.0f64, 216.0f64, 24.0f64);
    let row_h = 10.0f64; // font small's line height + 2
    let rows = 2usize;
    let container = h.create(TYPE_VIEW);
    let highlight = h.create(TYPE_VIEW);
    h.boxx(container, lx, ly, lw, lh);
    h.boxx(highlight, 0.0, 0.0, lw, row_h);
    h.ui.set_prop(highlight, P_BG_COLOR, 0x2a5f_8fffu32 as f64);
    h.ui.set_prop(highlight, P_RADIUS, 2.0);
    h.ui.set_prop(highlight, P_DISPLAY, 1.0);
    h.ui.insert_before(container, highlight, 0);
    let items: [(&str, Option<&str>); 2] = [("ALPHA", Some("1")), ("BRAVO", None)];
    for (r, &(text, extra)) in items.iter().enumerate().take(rows) {
        let label = h.create(TYPE_TEXT);
        let detail = h.create(TYPE_TEXT);
        h.boxx(label, 4.0, r as f64 * row_h + 1.0, lw - 8.0, row_h - 2.0);
        h.boxx(detail, lw / 2.0, r as f64 * row_h + 1.0, lw / 2.0 - 4.0, row_h - 2.0);
        h.ui.set_prop(label, P_FONT_SLOT, 0.0);
        h.ui.set_prop(detail, P_FONT_SLOT, 0.0);
        h.ui.set_prop(label, P_TEXT_COLOR, 0xf0f8_ffffu32 as f64);
        h.ui.set_prop(detail, P_TEXT_COLOR, 0xa9ba_caffu32 as f64);
        h.ui.set_prop(detail, P_TEXT_ALIGN, 2.0);
        h.ui.insert_before(container, label, 0);
        h.ui.insert_before(container, detail, 0);
        // list_render(): display first, then the text.
        h.ui.set_prop(label, P_DISPLAY, 0.0);
        h.ui.set_prop(detail, P_DISPLAY, if extra.is_some() { 0.0 } else { 1.0 });
        h.ui.set_text(label, text);
        if let Some(extra) = extra {
            h.ui.set_text(detail, extra);
        }
    }
    h.ui.insert_before(home, container, 0);
    h.ui.set_prop(highlight, P_DISPLAY, 0.0);
    h.ui.set_prop(highlight, P_INSET_T, 0.0);
    h.ui.insert_before(ROOT, home, 0); // pocket.ui.push(screen)
    if step == 6 {
        return (h.ui, h.nodes);
    }

    // A node created and removed: destroy_node has to give the budget back.
    let g = h.text(home, [14.0, 0.0, 212.0, 12.0], 255.0, 0.0, "g");
    h.ui.destroy_node(g);
    h.nodes -= 1;
    if step == 7 {
        return (h.ui, h.nodes);
    }

    // push(over) detaches the screen below; pop() destroys the top and
    // re-attaches it. Detach-and-reattach is the part no legacy app does.
    let over = h.create(TYPE_VIEW);
    h.boxx(over, 0.0, 0.0, LCD_W as f64, LCD_H as f64);
    h.ui.set_prop(over, P_BG_COLOR, 0x1a0f_24ffu32 as f64);
    let label = h.text(over, [16.0, 60.0, 208.0, 12.0], 0xffd4_79ffu32 as f64, 0.0, "SECOND");
    h.ui.remove_child(ROOT, home);
    h.ui.insert_before(ROOT, over, 0);
    h.ui.destroy_node(label);
    h.ui.destroy_node(over);
    h.nodes -= 2;
    h.ui.insert_before(ROOT, home, 0);
    if step == 8 {
        return (h.ui, h.nodes);
    }

    // The toast hangs off the root, above every screen.
    let tbox = h.create(TYPE_VIEW);
    h.boxx(tbox, 8.0, LCD_H as f64 - 26.0, LCD_W as f64 - 16.0, 18.0);
    h.ui.set_prop(tbox, P_BG_COLOR, 0x0b1c_2ee6u32 as f64);
    h.ui.set_prop(tbox, P_RADIUS, 4.0);
    h.ui.set_prop(tbox, P_Z_INDEX, 100.0);
    let tlabel = h.text(tbox, [6.0, 3.0, LCD_W as f64 - 28.0, 12.0], 0xf0f8_ffffu32 as f64, 2.0, "");
    h.ui.insert_before(ROOT, tbox, 0);
    h.ui.set_text(tlabel, "ようこそ");

    // `pad` more nodes, for asking "what if my screen had a few more?". This is
    // how the abort is reproduced: one past safeNodes and the relayout wants a
    // block the cap has not got, and the process dies exactly as the board did.
    for i in 0..pad {
        let extra = h.create(TYPE_VIEW);
        h.boxx(extra, (i % 60) as f64 * 4.0, 0.0, 2.0, 2.0);
        h.ui.set_prop(extra, P_BG_COLOR, 0xff00_00ffu32 as f64);
        h.ui.insert_before(home, extra, 0);
    }
    (h.ui, h.nodes)
}

fn main() {
    let only: Option<usize> = std::env::args().nth(1).and_then(|a| a.parse().ok());
    // Extra nodes on the last step, to walk a screen of your own past the cliff.
    let pad: u32 = std::env::args().nth(2).and_then(|a| a.parse().ok()).unwrap_or(0);
    let cap = cap_from_env();
    println!(
        "allocator refuses any single block over {} bytes\n",
        if cap == usize::MAX { "(no limit)".into() } else { cap.to_string() }
    );
    for (step, what) in STEPS.iter().enumerate() {
        if only.is_some_and(|o| o != step) {
            continue;
        }
        // The build is capped too: jsfont's atlas reload is a large block on
        // the device as well, and leaving it uncapped would hide it.
        let mut strip = Strip::new();
        arm();
        let (mut ui, nodes) = build(step, if step + 1 == STEPS.len() { pad } else { 0 });
        ui.tick();
        let words: Vec<u32> = ui.draw().words.clone();
        let software = render_frame(&ui, &words, &mut strip);
        disarm();
        println!(
            "step {step}: {what:<44} {nodes:>2} nodes  block {:>6}  words {:>4}  sw {:?}",
            biggest(),
            words.len(),
            software.unwrap_or(0)
        );
        // What pocket_ui.c's budget predicts, against what the run actually
        // asked for. The C counts every live node; taffy drops empty text runs
        // and anything not attached to the root, so the C over-counts and
        // refuses at or before the real cliff -- never after it, which is the
        // direction a guard has to err in.
        let predicted = layout_block(nodes);
        if predicted > cap {
            println!("  !! pocket_ui.c refuses this: its count needs {predicted}, cap is {cap}");
            if biggest() < predicted {
                println!("     (the run itself asked for only {}; the C is being", biggest());
                println!("      conservative, which is the safe way to be wrong)");
            }
        }
    }
    println!("\nno step aborted: every block this screen asks for fits the cap.");
}
