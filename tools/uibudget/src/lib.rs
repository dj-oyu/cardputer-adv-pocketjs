//! Shared pieces for the two binaries: an allocator that behaves like the
//! device's, a board configured like the firmware's, and the strip loop
//! app_session.c runs.
//!
//! Why any of this exists is in README.md. The short version: the Rust layout
//! asks for ONE contiguous block whose size steps with the node count, the
//! device's allocator answers NULL when no block that size is free, and Rust
//! aborts on that instead of reporting it. Neither the size nor the step can
//! be seen from the C side, so they are measured here.

use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering::Relaxed};

use pocketjs_core::Ui;
use pocketjs_render_rgb565::{PpaOps, Rect, Renderer, RendererConfig, SrmTransform};

// ---------------------------------------------------------------- the board

pub const LCD_W: f32 = 240.0;
pub const LCD_H: f32 = 135.0;
/// board.c hands the renderer one strip at a time, and this is its height.
pub const STRIP_H: u32 = 8;

/// Node types and property ids of the PocketJS core, from
/// .cache/pocketjs/engine/core/src/spec.rs. Same numbers main/pocket_ui.c uses.
pub const TYPE_VIEW: u8 = 0;
pub const TYPE_TEXT: u8 = 1;
pub const P_WIDTH: u8 = 1;
pub const P_HEIGHT: u8 = 2;
pub const P_POS_TYPE: u8 = 24;
pub const P_INSET_T: u8 = 25;
pub const P_INSET_L: u8 = 28;
pub const P_DISPLAY: u8 = 29;
pub const P_OVERFLOW: u8 = 30;
pub const P_Z_INDEX: u8 = 31;
pub const P_BG_COLOR: u8 = 64;
pub const P_RADIUS: u8 = 68;
pub const P_TEXT_COLOR: u8 = 96;
pub const P_FONT_SLOT: u8 = 97;
pub const P_TEXT_ALIGN: u8 = 98;
pub const ROOT: i32 = 1;

/// A core configured the way app_start_test() configures it, with the same
/// three font slots: 0 and 1 are the generated latin atlases, 2 is jsfont.c's
/// dynamic Japanese slot holding `jp` (plus the tofu at gid 0).
pub fn board(jp: &[char]) -> Ui {
    let mut ui = Ui::new_with_raster_density(1);
    ui.set_viewport(LCD_W, LCD_H);
    assert!(
        ui.load_font_atlas(include_bytes!(concat!(env!("OUT_DIR"), "/font_small.bin"))),
        "slot 0 rejected"
    );
    assert!(
        ui.load_font_atlas(include_bytes!(concat!(env!("OUT_DIR"), "/font_large.bin"))),
        "slot 1 rejected"
    );
    // jsfont_attach() loads the tofu-only slot before a program runs and
    // reloads the whole slot whenever a new character appears; only the final
    // shape matters to the layout, so it is loaded once here.
    assert!(ui.load_font_atlas(&jp_atlas(jp)), "slot 2 rejected");
    ui
}

/// jsfont.c's reload(), in Rust: DCFA v3, the tofu at gid 0 mapped from U+0000
/// and then one fixed 12x12 cell per codepoint, ascending.
///
/// The cells are a hollow box rather than the real shinonome glyphs: the shape
/// of the ink changes what the strips look like, and changes nothing about how
/// much memory the layout asks for, which is what this harness measures.
pub fn jp_atlas(chars: &[char]) -> Vec<u8> {
    let (cw, ch) = (12usize, 12usize);
    let cell = cw * ch;
    let mut sorted: Vec<u32> = chars.iter().map(|&c| c as u32).collect();
    sorted.sort_unstable();
    sorted.dedup();
    let count = sorted.len() + 1;
    let mut b = vec![0u8; 16 + count * 8 + count * cell];
    b[0..4].copy_from_slice(&0x4146_4344u32.to_le_bytes()); // 'DCFA'
    b[4] = 3; // version
    b[6..8].copy_from_slice(&(count as u16).to_le_bytes());
    b[8] = cw as u8;
    b[9] = ch as u8;
    b[10] = 10; // baseline
    b[11] = ch as u8; // line advance
    b[12] = 2; // slot
    b[14] = 1; // density
    let cmap = 16;
    let cover = cmap + count * 8;
    b[cmap + 6] = cw as u8; // gid 0 advance
    for (i, cp) in sorted.iter().enumerate() {
        let e = cmap + (i + 1) * 8;
        b[e..e + 4].copy_from_slice(&cp.to_le_bytes());
        b[e + 4..e + 6].copy_from_slice(&((i + 1) as u16).to_le_bytes());
        b[e + 6] = cw as u8;
        for y in 0..ch {
            for x in 0..cw {
                if y == 0 || y == ch - 1 || x == 0 || x == cw - 1 {
                    b[cover + (i + 1) * cell + y * cw + x] = 255;
                }
            }
        }
    }
    b
}

// ------------------------------------------------------------- the allocator

static LIVE: AtomicUsize = AtomicUsize::new(0);
static PEAK: AtomicUsize = AtomicUsize::new(0);
static BIGGEST: AtomicUsize = AtomicUsize::new(0);
static ARMED: AtomicUsize = AtomicUsize::new(0);
static CAP: AtomicUsize = AtomicUsize::new(usize::MAX);

/// Stands in for pocketjs_idf_rust_alloc: with a cap set it refuses any single
/// block larger than the device's largest free block, which is the one thing
/// about the device's heap that decides whether a screen lives or reboots.
/// Fragmentation, the total free size and the guest's own heap are NOT modelled
/// — see README.md, "前提と限界".
pub struct Track;

unsafe impl GlobalAlloc for Track {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        if l.size() > CAP.load(Relaxed) {
            return std::ptr::null_mut();
        }
        if ARMED.load(Relaxed) == 1 {
            BIGGEST.fetch_max(l.size(), Relaxed);
            let live = LIVE.fetch_add(l.size(), Relaxed) + l.size();
            PEAK.fetch_max(live, Relaxed);
        }
        System.alloc(l)
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        if ARMED.load(Relaxed) == 1 {
            LIVE.fetch_sub(l.size().min(LIVE.load(Relaxed)), Relaxed);
        }
        System.dealloc(p, l)
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, new: usize) -> *mut u8 {
        if new > CAP.load(Relaxed) {
            return std::ptr::null_mut();
        }
        if ARMED.load(Relaxed) == 1 {
            BIGGEST.fetch_max(new, Relaxed);
            let live = LIVE.fetch_add(new.saturating_sub(l.size()), Relaxed) + new;
            PEAK.fetch_max(live, Relaxed);
        }
        System.realloc(p, l, new)
    }
}

/// The largest free block a Cardputer ADV reports once a JS guest is up,
/// measured on the device (`app: MEM ... largest=23552`). It is the default
/// ceiling because it is the number that decides the question.
pub const DEVICE_LARGEST_FREE: usize = 23552;

/// Refuse any single block over `bytes`. `usize::MAX` for an unlimited host.
pub fn set_cap(bytes: usize) {
    CAP.store(bytes, Relaxed);
}
/// Read the cap from POCKETUI_CAP, defaulting to the device's. Returns it.
pub fn cap_from_env() -> usize {
    let cap = match std::env::var("POCKETUI_CAP") {
        Ok(v) if v == "none" => usize::MAX,
        Ok(v) => v.parse().expect("POCKETUI_CAP must be a number or `none`"),
        Err(_) => DEVICE_LARGEST_FREE,
    };
    set_cap(cap);
    cap
}

pub fn arm() {
    LIVE.store(0, Relaxed);
    PEAK.store(0, Relaxed);
    BIGGEST.store(0, Relaxed);
    ARMED.store(1, Relaxed);
}
pub fn disarm() {
    ARMED.store(0, Relaxed);
}
/// The largest single block asked for since arm(). This is the number the
/// device's allocator either has or does not have.
pub fn biggest() -> usize {
    BIGGEST.load(Relaxed)
}
pub fn peak() -> usize {
    PEAK.load(Relaxed)
}

// ---------------------------------------------------------------- rendering

/// Declines everything, which is what main/render_accel.c does for any op its
/// two hand-written kernels cannot honour exactly. Declining ALL of them puts
/// every op through the Rust software path, which is the half a host can run.
pub struct NoPpa;

impl PpaOps for NoPpa {
    fn fill_rgb565(&mut self, _: &mut [u16], _: u32, _: u32, _: Rect, _: u16) -> bool {
        false
    }
    fn blend_a8_rgb565(
        &mut self,
        _: &mut [u16],
        _: u32,
        _: u32,
        _: &[u8],
        _: Rect,
        _: [u8; 3],
        _: u8,
    ) -> bool {
        false
    }
    fn srm_psm5650_to_rgb565(
        &mut self,
        _: &mut [u16],
        _: u32,
        _: u32,
        _: &[u8],
        _: u32,
        _: u32,
        _: Rect,
        _: Rect,
        _: SrmTransform,
    ) -> bool {
        false
    }
}

/// board.c's one strip buffer, which the whole firmware shares. It is a
/// separate type so a caller allocates it BEFORE arm(): a fresh Vec per strip
/// would put 3,840 bytes of the harness's own making into every measurement and
/// read as if the device paid for it.
pub struct Strip(Vec<u16>);

impl Strip {
    pub fn new() -> Strip {
        Strip(vec![0u16; LCD_W as usize * STRIP_H as usize])
    }
}

impl Default for Strip {
    fn default() -> Strip {
        Strip::new()
    }
}

/// One frame the way app_tick() draws it: full-width strips of STRIP_H rows,
/// bottom strip short. Returns the software op count, or None if a strip was
/// declined.
pub fn render_frame(ui: &Ui, words: &[u32], strip: &mut Strip) -> Option<u32> {
    // The renderer is persistent in the firmware too; it is built here rather
    // than by the caller because it holds no large buffer until it draws.
    let mut renderer = Renderer::new(RendererConfig {
        scale: 1,
        min_fill_pixels: 1,
        min_blend_pixels: 1,
        min_srm_pixels: 1,
    })?;
    let mut ppa = NoPpa;
    let mut software = 0;
    let mut y = 0;
    while y < LCD_H as u32 {
        let rows = (LCD_H as u32 - y).min(STRIP_H);
        // The bottom strip is short (135 is not a multiple of 8), and board.c
        // passes the same buffer with a smaller length.
        let pixels = &mut strip.0[..LCD_W as usize * rows as usize];
        let region = Rect { x: 0, y, w: LCD_W as u32, h: rows };
        software += renderer.render_strip(ui, words, pixels, region, &mut ppa)?.software_ops;
        y += rows;
    }
    Some(software)
}

/// The step function main/pocket_ui.c's layout_block() encodes, so the C and
/// the harness cannot disagree about where the cliffs are. `nodes` excludes the
/// root, which the core owns.
pub fn layout_block(nodes: u32) -> usize {
    let taffy = nodes + 1;
    if taffy <= 16 {
        0
    } else if taffy <= 33 {
        29648
    } else {
        59296
    }
}
