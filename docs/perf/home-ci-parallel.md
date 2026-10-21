# Complete HOME host suite scheduling

Base integration: a1a93afbae73ca582ddc163166275dc2de7c06c2 (PR42).
Only host scripts, workflow wiring and these notes change. Firmware sources,
configuration defaults and the hardware comparison source remain unchanged.

The existing pull-request workflow keeps one complete host job. Its internal
entry runs every former command/configuration:

| Label | Existing run_derby.py options | Scripted cases |
| --- | --- | ---: |
| derby-asan | default ASan/UBSan | 14 |
| derby-telemetry | --telemetry --heap-limit 524288 | 1 |
| derby-measure | --measure --heap-limit 524288 | 1 |
| derby-m32 | --m32 | 11 |
| derby-m32-telemetry | --m32 --telemetry --heap-limit 163840 | 1 |

The device-sized measure-accounting test, ordinary HOME/console contracts,
record_verification receipt and firmware job remain. Other workflows retain
ASan/O2, capture, normal/m32 games, baseline/current-source overlay/tree proofs
and all-success gates. There is no selectable fast/full test mode.

## Inputs and outputs

Lowered app, diagnostic chunks, ROM/font/subscription fixtures and fresh binary
are generated in a stable ABI/telemetry/measure/course profile. A profile lock
covers generation and all consumers, so another invocation cannot rewrite a
borrowed executable or fixture. Different profiles own different directories.
Compiler objects alone remain dependency-validated and shared by host_build;
no random include path invalidates otherwise reusable compiler inputs.

Each invocation clears its profile's results directory under that lock. Each
case owns a numbered CSV path; demo/seed replays and preview output have their
own paths. Preview exports retain their shared lock because the documented PNG
destination names are fixed across profiles. m32 provisioning remains before
its consumers; this script also serializes its own sysroot-preparation calls.
Existing standalone run_derby.py options remain available with full checks.

All fixture generation/linking finishes before worker submission. At most two
scripted cases execute concurrently, followed by the MID-output-dependent demo
and seed checks. The original cross-case finish equality gate is retained.
Straight/oval20,000-sample odds and500-seed bend checks execute in a second,
bounded two-worker phase. Results/logs are consumed in original order. Worker
errors and cross-case mismatches still fail the suite. Other full-contract
lanes already run concurrently; hosted-runner effects must be measured rather
than assuming nested parallelism is always faster.

## Source-only Node checks

The lane/photo/pan-order Node commands read original apps/derby files and their
own scripts. They do not read the lowered app, generated diagnostic chunk,
compiled binary, ABI, heap limit or profile directory. In the complete HOME
invocation these exact three checks run once before the five profiles. The
Node executable, runner/test scripts, all original app files and relevant Node
options are fingerprinted before/after validation and around every profile.
Changed inputs fail that invocation. Standalone run_derby.py still runs the
three checks unconditionally. No persisted success stamp or proof is read.

Per-mode logs retain their former artifact names. derby-complete.json is a
fresh output with the exact source, Node fingerprint, original mode options
and each successful duration. It is written only after every mode passes and
is never cached. An early failure removes old logs/proof, preventing stale
subset results from appearing as current complete coverage.

## Validation and timing

Boundary tests freeze the original14-case selection and all oracle/m32/heap
filters, all five HOME options, profile/CSV isolation, writer exclusion,
preview lock order, fixture-before-reader dependency, stale-output removal,
Node-source mutation, late worker/config failures and finish mismatch.

Before this change, head8863acf5 HOME host took9:44; Node+ASan4:47, telemetry54s,
measure53s, m322:06. Earlier head1e68c100 took13:12 on another hosted runner.
Most normal ASan time was runtime checking, so compiler cache alone cannot
eliminate it. Compare the ordinary complete CI on the published exact head;
no measurement-only run is required. Cross-run runner variation remains a
material limitation. Existing compiler cache can restore from the integration
push at a1a93afb; test output and receipts must still be regenerated.
