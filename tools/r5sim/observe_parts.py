"""Group observe.py's --json by part (docs/vm/qjs-observe.md sec.2).

    python tools/r5sim/observe_parts.py obs.json qjs_funcs.txt
qjs_funcs.txt: the defined functions of quickjs.c.obj, one per line
(nm --defined-only <build>/esp-idf/quickjs-ng/.../quickjs.c.obj | awk '$2~/[tTwW]/{print $3}').
The part rules are names; a boundary call moves a few hundred instructions.
"""
import json,re,collections,sys
d=json.load(open(sys.argv[1])); n=d['turns']
ins=d['insn_by_func']; tk=d['taken_by_func']; lu=d['loaduse_by_func']; l32=d['l32r_by_func']
qjs=set(open(sys.argv[2]).read().split())
def part(f):
    if f.startswith('_Window'): return 'window exceptions'
    if re.match(r'^(tlsf_|heap_caps_|multi_heap_|xPortEnterCritical|vPortExitCritical|vPortEnterCritical|xPortInIsrContext|_xtos_set_intlevel|__call__xtos|esp_cpu_compare_and_set|assert_valid_block|block_cache_|r5_|guest_malloc|guest_free|guest_calloc|guest_realloc|guest_usable_size|guest_uses_block_cache|guest_block_length|xTaskGetCurrentTaskHandle|js_malloc_rt|js_free_rt|js_realloc_rt|js_calloc_rt|js_def_|js_malloc$|js_free$|js_mallocz|esp_heap_adjust)',f): return 'allocator'
    if re.match(r'^__(.*[sd]f[23]|.*di3|float|fix|extend|trunc)',f): return 'libgcc soft-double/64bit'
    if re.match(r'^(__kernel_|__rem_pio2|__ieee754|sin$|cos$|floor|ceil|fmod|sqrt|round|trunc|scalbn|copysign|fabs)',f): return 'libm (double)'
    if re.match(r'^(__call_)?(memcpy|memset|memcmp|memmove|strlen|strcmp|__memcpy_aux|bzero)$',f): return 'mem*/str*'
    if f in qjs: return 'quickjs.c'
    if re.search(r'Isr|Tick|tick|systimer|_xt_|xt_|UserException|Interrupt|_frxt|vTask|xTask|esp_timer|usb_serial|Idle',f): return 'rtos/isr'
    return 'kasane/pocket/other'
P=collections.Counter(); Pt=collections.Counter(); Pl=collections.Counter(); Pr=collections.Counter()
for f,v in ins.items():
    p=part(f); P[p]+=v; Pt[p]+=tk.get(f,0); Pl[p]+=lu.get(f,0); Pr[p]+=l32.get(f,0)
tot=sum(P.values()); cyc=sum(P[p]+2*Pt[p]+Pl[p] for p in P)
print(f"{'part':26s} {'insn/turn':>10s} {'%insn':>6s} {'taken':>8s} {'loaduse':>8s} {'l32r':>7s} {'model cyc':>10s} {'%':>6s}")
for p,v in P.most_common():
    c=v+2*Pt[p]+Pl[p]
    print(f"{p:26s} {v/n:10,.0f} {v/tot:6.1%} {Pt[p]/n:8,.0f} {Pl[p]/n:8,.0f} {Pr[p]/n:7,.0f} {c/n:10,.0f} {c/cyc:6.1%}")
print(f"{'total':26s} {tot/n:10,.0f}        {sum(Pt.values())/n:8,.0f} {sum(Pl.values())/n:8,.0f} {sum(Pr.values())/n:7,.0f} {cyc/n:10,.0f}")
x=d['xfer']; print('calls/turn', x.get('call',0)/n, 'rets', x.get('ret',0)/n)
