# Shared base object caches

The complete contract workflow also runs on normal pushes to `main` and
`integration/derby-j1-dma-host-r2-20261020`. Its same baseline/current-source
matrix, ASan/O2, capture, normal/m32 games and source/tree/overlay proofs run;
only dependency-validated compiler objects are cached. Images, executables,
fixtures, success stamps and proof artifacts are rebuilt each time.

[GitHub cache scopes](https://docs.github.com/en/actions/reference/workflows-and-actions/dependency-caching#restrictions-for-accessing-a-cache)
allow PRs to inherit default/base branch caches. A cache saved under one PR's
merge ref is not available to another PR. PR41, PR37 and PR42 having the same
helper hash and restore prefix therefore did not establish a warm shared cache.
A normal base push can seed it without manual dispatch or a measurement run.
No new privileged event, permissions, secrets or network configuration is used.
The helper still validates compiler identity, dependency contents, flags, ABI
and object integrity, then freshly links and executes every acceptance case.

The existing ref-based cancellation is retained. A newer base push can cancel
an obsolete push, but a PR update cannot cancel base-scope cache publication.
Push and PR checks are both retained: with an open hub PR such as #37, an
integration push also updates that PR and costs one extra complete workflow
(two source jobs). Current PRs must not appear green merely because tests were
skipped. This deliberately small change does not attempt cross-event result
reuse, which would need to prove both workflow and tested-tree equivalence.
Feature-branch pushes do not trigger an extra workflow.

The first inheritable cache is only established after an approved normal base
update completes successfully. This change does not merge a branch or dispatch
a seed run. Subsequent cache restoration and complete step durations must be
observed before claiming a cross-PR speedup. HOME currently has no Actions
object cache; its Node/ASan and m32 work remains a separate critical path.
Runner variation and changed test load must be distinguished from cache scope.
