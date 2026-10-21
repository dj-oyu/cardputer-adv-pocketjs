# Prepared API input layout experiment

Base: integration `c636b14d86ae75dc6234a49cacca2f0f6427e5eb`.

The goal is to replace generic preparation with data that a dedicated online
entry can consume directly, reducing reconstruction, type adaptation, packing
and memory writes on the VM startup path. An analyzer or a more precise type
system is not the goal. Here **offline means preparation phase**, not a PC,
network or deployment requirement. This first host experiment produces a
layout plus prepared literal float32 inputs. **It is not connected to the VM
call dispatcher or an artifact loader. No runtime speed or memory improvement
has been established.** Existing execution still takes the generic path.

## Concrete path and intended boundary

The representative API is the real
`pocket.kasane.procedural.register(name, args?, points?)`, specifically its ROM
plan overload. `probe.c` includes the actual `main/pocket/pocket_proc.c` and
installs its actual native functions. It supplies small valid ROM entries as
fixtures through the existing `pocket_proc_rom_plans()` registry API. The
synthetic entries are test inputs, not an alternative implementation of the
system API and not a production registry loader.

Current path:

1. QuickJS compiles/evaluates the app chunk; execution resolves the callee and
   creates/evaluates JS arguments (including a JS array for literal args).
2. `js_register` / `guarded` / `register_rom` check the call, convert the plan
   name, look up the ROM entry, read array length and check the slot limit.
3. The adapter allocates `ksn_proc_rom_plan_bytes(params)`, reads JS properties
   into numbers, writes float args and checks instruction-field domains.
   A `double arg[8]` staging array is used by this existing generic path.
4. `ksn_proc_rom_plan_prepare()` reconstructs patched code in its normal
   scratch and analyzes it, then a slot owns the resulting native plan. The
   result is the existing JSValue handle or exception.

This experiment moves *planning* of known literal inputs to preparation:
actual entry validity, exact parameter count, finite Number/float32 domains,
patch-field constraints, byte count, alignment, offsets and float32 bit values.
It invokes the existing `ksn_proc_rom_plan_prepare()` against bounded temporary
native storage to check the plan; it does not invent another plan validator.
The minimum request comes from the actual C layout/size function:

`max(sizeof(ksn_proc_rom_plan), offsetof(args) + params * sizeof(float))`.

On the checked 32-bit ABI this is 40 + 4 * params bytes, aligned to 4. A 64-bit
report is explicitly a different ABI (including its sizeof floor/padding) and
must not be installed as a device layout. These are allocator request sizes,
not total heap consumption or an allocator's rounded block/header size.
Prepared float32 bit strings are constant input data; no raw native pointers
or host struct images are serialized.

Online must retain callee/property evaluation, all observable argument side
effects in their original order, actual captured-callee identity validation,
registry lifetime/generation validation, slot/handle limit precedence, OOM and
error cleanup, ownership and the normal JSValue return. This API ignores its
receiver; that is not an assumption valid for every system API. Unknown calls,
dynamic inputs, unsupported expressions and failed guards retain generic
behavior. A guard must consume the callee already captured by the ordinary VM;
a fresh global lookup before or after the call is not an identity proof.

The missing next-stage entry would reserve the **existing** native allocation,
consume prepared float inputs directly, and initialize the existing plan
without first constructing generic argv/arrays or a double staging vector.
Known constant patch/analysis results may also be reusable after target ABI and
registry validation. The current prototype writes native float slots directly
in its preparation scratch but does not eliminate any online writes yet.
A real connection must preserve generic fallback and allocation/error ordering;
particularly, omitting JS-array allocation can alter OOM precedence and cannot
be assumed behaviorally free. No second large runtime pool is proposed.

## Parser and proof limits

`bytecode.c` includes the existing QuickJS implementation in a **host-only**
translation unit, as the existing vmtest/floor probes do. `JS_Eval` with
`JS_EVAL_FLAG_COMPILE_ONLY` supplies the real lexer/parser/compiler. The analyzer
uses that engine's opcode definitions and constant pool; it does not implement
JS grammar or scan source for function-name strings. Small abstract facts
cover literals and one candidate property path. Unsupported transformations
are barriers; branch-bearing functions are conservative. Call results remain
unknown: there is no return-type inference.

A path match only selects a **guarded candidate**. It never authorizes a fast
call. The identity unit test checks the rooted actual native function object,
native entry pointer, C calling convention, realm and immutable test registry.
A same-name replacement, another function object for the same native entry,
a foreign realm, or another registry fails. GC cannot release the held binding.
A namespace getter returning the actual function can still have side effects;
identity does not authorize skipping that getter. The original program's
unchanged generic execution verifies getter/argument effects occur exactly
once. The identity tests exercise a simulated dispatch boundary, not a live
specialized VM dispatcher. Every unreported call site defaults to generic.

Analysis reuses one fixed 64-value scratch for at most 16 functions / 32 sites;
exceeding a bound rejects all planned sites. Source input is capped at 16 KiB
and the host compiler runtime uses a 16 MiB allocation limit. The reported
abstract scratch excludes the ordinary QuickJS compiler heap/compiled artifact.
There is no additional firmware reservation. Syntax/compile failure emits no
usable plan. Current reports are experimental diagnostics/prepared inputs,
not an authenticated loadable format. A future loader must bind source,
compiler/opcode ABI, target ABI and trusted native-contract/ROM identities;
reported names or SHA text alone are not runtime binding authority.

Execution JSValue width, segment stacks, flat calls, argv borrowing, GC,
closures, suspend/resume and cleanup are unchanged. The experiment has no
product build inclusion and does not modify QuickJS or system API sources.
An API redesign, if needed next, should expose a small trusted contract/binding
descriptor and the existing allocation initializer, not an independently
maintained full API type system.

## Verification and relation to existing work

`python3 tools/vmtest/prepare/run.py` runs native O2 and ASan/UBSan contracts.
The existing complete suite also invokes `--m32` for the device-width layout.
This is an internal ABI test, not a fast/full workflow choice. Fixtures cover
known/zero/variable inputs, unknown calls, same-name replacement, arity/count
and type failures, dynamic arrays, overflow, side effects, getters, closures,
branches, spread, strings/regex lookalikes, malformed syntax and analysis bounds.
Known single-call fixtures compare the actual generic allocation and stored
float bits with the prepared plan. Other fixtures run their unchanged generic
program to check allocation/error/side-effect behavior. No fixture execution
happens in the preparation pass. All reports are regenerated; only compiler
objects use the common dependency-aware host cache. Per-ABI output locks avoid
same-path concurrent readers/writers, and m32 provisioning uses the existing
shared lock/sysroot. A shorter existing C lane carries this test without adding
parallel workers. Baseline and current sources receive the declared common
harness overlay, hash it and retain their original product source identities.

Existing ROM plans already move instructions to flash. This work consumes their
contract/layout; it must not count their prior savings again. PR24's validated
plan direction overlaps the native validation step, while J0 batching addresses
call counts. This is a separate host experiment, not a decision to adopt or
replace either branch. Production adoption and the dedicated execution entry
remain unimplemented.

There are no startup CPU/wall-clock or memory-I/O speed measurements here.
Old vmprobe steady-turn/call figures are not a current boot baseline, and
HEAPPROBE eval_us includes logging between its clock start and evaluation.
Neither supports a performance claim for this prototype. No measurement-only
workflow/PR or new profiler is introduced.
