# First synchronous prepared-input connection

This is a limited runtime connection above integration `578d552`, separate
from PR46 allocation/header work and PR47 app control. It does not establish
a speed, heap or memory-write improvement. DERBY's existing dynamic `load()`
call is still generic: branches, aliases and dynamic names/vectors are outside
the bounded recognizer. The first supported startup fixture is
`tools/vmtest/prepare/fixtures/runtime-startup.js`, using the actual generated
`derby.pt` ROM entry. Its input constants are ordinary fixture JS, not special
cases embedded in the consumer.

`eval_reporting()` now sends global scripts through
`pocket_proc_eval_prepared()`. Modules keep their existing loader. Sources
over 16 KiB or without a ROM registry use ordinary evaluation. Smaller global
scripts use the engine's compile-only/evaluate-function pair. An allocation-free
recognizer reads the same compiler's opcode boundaries, atoms and constant
pool; it accepts one contiguous direct property path, literal name and up to
eight literal Numbers. Branch-bearing roots, multiple candidates, closures,
spread and unsupported forms are generic. This is a narrower runtime subset
of the host experiment, not a second JS grammar or return-type inference.

The generated descriptor contains a bytecode identity, selected lookup and
call PCs, name, doubles and target-compiled float values. It lives on the
synchronous loader's stack. It is not serialized, persisted, authenticated
external input or a native host struct installed on a different ABI.
The running ordinary VM owns the compiled bytecode through evaluation;
no descriptor or bytecode pointer survives the loader call.

The original native register function keeps its calling convention, lazy
creation, property attributes and allocation shape. A private magic value
identifies its function-list definition. Only lazy instantiation at the exact
selected bytecode lookup captures a binding reference. Earlier lookups do not
retain otherwise dead functions through later allocation. A preexisting or
getter-returned binding has no such proof and takes ordinary dispatch. The
reference is released immediately after the selected dispatch or guard decline;
evaluation cleanup is the exception safety net.

At the method-call opcode, a private internal flag records that the captured
operand itself is the marked native object. Bound functions and Proxy
forwarding never acquire that flag. The native hook runs after the existing
stack check, frame setup, realm selection and argv padding. It checks the
rooted captured object, context, owner/PC, arity and immutable registry identity.
No global/property relookup substitutes a different function. Failed guards
fall through to ordinary dispatch once. No JSValue, GC, bytecode, frame,
runtime/context layout or public API ABI is changed.

The selected initializer preserves name conversion, count/slot limits and
native calloc before reading argument elements. It reads each original element
once. Matching Numbers consume generated float inputs. Changed Numbers use
the already-read scalar as their fallback, with field-domain errors deferred
until all numeric reads finish, as in the existing adapter. It never invokes
register again or rereads a getter. The ordinary plan validator, points
attachment, handle publication, cleanup and allocation ownership remain in
place. JS argv and literal arrays are still allocated normally, including OOM.
The single-owner procedural adapter's existing task confinement also applies
to the synchronous scope chain; this adds no background task or runtime pool.

## Verification and limits

`python3 tools/vmtest/prepare/run.py` runs the existing host experiment and
the new actual loader/dispatcher contract on native O2 and ASan/UBSan.
`--m32` runs both on the device-width ABI in the complete CI contract suite.
The common test overlay still runs the original contracts against the pinned
older baseline; runtime-specific contracts run only where the product
connection exists. Runtime success requires an observed prepared hit and the
actual native plan's input values, not a requested execution-mode flag.

Regressions cover the generated ROM fixture, one-read scalar fallback,
native calloc before getters, original JS argument-array allocation failure,
root/GC lifetime, lookup/call PC guards, replacement/restoration, different
native objects, foreign contexts, registry replacement, bound/Proxy forwarding,
throw cleanup and unsupported control flow. Fault-injected JS allocations are
compared with ordinary evaluation, including success and array-OOM outcomes.

Native GCC O2 `-fstack-usage` gives `register_impl` 976 B before and 992 B
after, `js_register` 16 B in both, the new private dispatcher 32 B and the
loader wrapper 352 B. These are individual 64-bit host compiler frames, not
Xtensa/m32 values, complete call-chain peaks or a stack reduction. Existing
engine stack-limit checks remain active; target stack acceptance and hardware
performance/resource measurements are separate. The added recognition and
guards have CPU cost; this Draft connection is not evidence of net benefit.
