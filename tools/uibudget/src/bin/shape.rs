//! What the layout's big contiguous block actually scales with.
//!
//! The first sweep varied only the total node count and found a step at 16,
//! but a 16-node tree shaped like a real screen does not take that step. So
//! this varies the shape independently: how many children one parent has, how
//! deep the tree is, and how many nodes there are in total.
use pocketjs_core::Ui;
use uibudget::*;

#[global_allocator]
static A: Track = Track;

fn leaf(ui: &mut Ui, parent: i32, i: u32) -> i32 {
    let id = ui.create_node(TYPE_VIEW);
    ui.set_prop(id, P_POS_TYPE, 1.0);
    ui.set_prop(id, P_INSET_L, (i % 20) as f64 * 11.0);
    ui.set_prop(id, P_INSET_T, (i / 20) as f64 * 6.0);
    ui.set_prop(id, P_WIDTH, 8.0);
    ui.set_prop(id, P_HEIGHT, 4.0);
    ui.set_prop(id, P_BG_COLOR, 0xff00_00ffu32 as f64);
    ui.insert_before(parent, id, 0);
    id
}

/// `fanout` children per parent, `total` nodes, breadth-first under the root.
fn measure(total: u32, fanout: u32) -> (usize, u32) {
    let mut ui = board(&[]);
    let mut parents = vec![ROOT];
    let mut next = Vec::new();
    let mut made = 0;
    let mut depth = 1;
    'outer: while made < total {
        next.clear();
        for &p in &parents {
            for _ in 0..fanout {
                if made == total {
                    break 'outer;
                }
                next.push(leaf(&mut ui, p, made));
                made += 1;
            }
        }
        parents = next.clone();
        depth += 1;
        if parents.is_empty() {
            break;
        }
    }
    let mut strip = Strip::new();
    arm();
    ui.tick();
    let words: Vec<u32> = ui.draw().words.clone();
    let _ = render_frame(&ui, &words, &mut strip);
    disarm();
    (biggest(), depth)
}

fn main() {
    set_cap(usize::MAX);
    println!("{:>6} {:>7} {:>7} {:>16}", "total", "fanout", "depth", "biggest block");
    // Same total, different shapes: if the block follows the total, every row
    // of a group matches; if it follows the widest parent, only fanout matters.
    for total in [8u32, 12, 15, 16, 17, 20, 24, 32, 33, 40] {
        for fanout in [2u32, 4, 8, 16, 64] {
            if fanout > total {
                continue;
            }
            let (block, depth) = measure(total, fanout);
            println!("{total:>6} {fanout:>7} {depth:>7} {block:>16}");
        }
        println!();
    }
}
