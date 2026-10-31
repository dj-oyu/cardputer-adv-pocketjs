"""Adapt pinned observer harnesses to added product helpers and owner hooks.

Only copied diagnostic sources are changed; oracle comparisons stay intact.
Unknown or ambiguous support/source revisions are rejected.
"""
from pathlib import Path
import argparse


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError("expected one pinned support anchor: " + old)
    return text.replace(old, new)


def plain(path):
    text = path.read_text()
    # The observer duplicates the whole band-render section in one C unit.
    # New analytic helpers must get the same private ref_/cand_ namespaces.
    text = once(text,
        "('band_entry','plain_band','pattern_band','render_band_ext','ksn_proc_render_band')",
        "('band_entry','plain_band','pattern_band','render_band_ext','ksn_proc_render_band',"
        "'ell_finite','ell_unit','ell_colour','ellipsoid_band','ksn_proc_ellipsoid_bounds',"
        "'ksn_proc_blend_read','ksn_proc_blend_bounds','blend_projection','blend_ellipsoids_band',"
        "'ksn_proc_blend_region_read','ksn_proc_blend_region_bounds',"
        "'clip_prepare','clip_contains','clip_span','ksn_proc_clipped_ellipsoid_read',"
        "'ksn_proc_clipped_ellipsoid_bounds')")
    text = once(text, "bool plain_step_candidate;\n", """bool plain_step_candidate;
bool ksn_proc_ellipsoid_bounds(const ksn_proc_ellipsoid *p,int *l,int *t,int *r,int *b){
 return cand_ksn_proc_ellipsoid_bounds(p,l,t,r,b);
}
bool ksn_proc_blend_bounds(const ksn_proc_blend_ellipsoids *p,int *l,int *t,int *r,int *b){
 return cand_ksn_proc_blend_bounds(p,l,t,r,b);
}
bool ksn_proc_blend_read(const ksn_proc_segment *data,unsigned words,ksn_proc_blend_ellipsoids *p,unsigned *used){
 return cand_ksn_proc_blend_read(data,words,p,used);
}
bool ksn_proc_blend_region_read(const ksn_proc_segment *data,unsigned words,ksn_proc_blend_ellipsoids *p,
        ksn_proc_blend_region_mask *mask,unsigned *used){
 return cand_ksn_proc_blend_region_read(data,words,p,mask,used);
}
bool ksn_proc_blend_region_bounds(const ksn_proc_blend_ellipsoids *p,const ksn_proc_blend_region_mask *mask,
        int *l,int *t,int *r,int *b){
 return cand_ksn_proc_blend_region_bounds(p,mask,l,t,r,b);
}
bool ksn_proc_clipped_ellipsoid_read(const ksn_proc_segment *data,unsigned words,ksn_proc_clipped_ellipsoid *p){
 return cand_ksn_proc_clipped_ellipsoid_read(data,words,p);
}
bool ksn_proc_clipped_ellipsoid_bounds(const ksn_proc_clipped_ellipsoid *p,int *l,int *t,int *r,int *b){
 return cand_ksn_proc_clipped_ellipsoid_bounds(p,l,t,r,b);
}
""")
    path.write_text(text)


def diagnostic(path, target):
    text = path.read_text()
    source = target.read_text()
    legacy = "    while(1) {\n        /* App teardown"
    observed = "    while(1) {\n        native_observe();"
    if source.count(legacy) == 1:
        return  # The pinned baseline retains the original exact anchor.
    if source.count(observed) != 1:
        raise ValueError("unknown or ambiguous UI owner-loop anchor")
    text = once(text,
        repr(legacy) + "," + repr("    while(1) {\n        diag_owner_step();\n        /* App teardown"),
        repr(observed) + "," + repr("    while(1) {\n        diag_owner_step();\n        native_observe();"))
    path.write_text(text)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kind', choices=('plain', 'diagnostic'))
    parser.add_argument('support', type=Path)
    parser.add_argument('--target', type=Path)
    args = parser.parse_args()
    if args.kind == 'plain':
        plain(args.support)
    else:
        if args.target is None:
            parser.error('--target is required for diagnostic')
        diagnostic(args.support, args.target)
