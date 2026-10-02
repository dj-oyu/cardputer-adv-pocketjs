# Historical isolated cloud validation on 89a71ae

These are the original independent candidate reports, preserved during consolidation. They are not combined-branch validation results. See `integrated-validation.md` for the integration report.

## A frame entry

Candidate: a-frame-entry. Base: 89a71ae72c721206dfbb63acfeffc74b38dde604.

The complete isolated candidate from vm/cloud-a-frame-entry was applied to this base with Git three-way application. Host focused O2 and AddressSanitizer/UndefinedBehaviorSanitizer tests were rerun and passed on 2026-10-01. Source changes from upstream are preserved. This is an independent candidate, not a combined integration. Earlier reports describe their original baseline; only the tests stated here were rerun.

LeakSanitizer was disabled because of the environment restriction. ESP-IDF build, hardware, runtime heap, FPS, and device timing are not verified. No COM3 access. No main/vm/main merge.

F precompile remains opt-in build-only; runtime loading is unchanged. A retains the documented bound/proxy limitation and does not fix the separately documented baseline lazy-initialization OOM issue.

## C parser shrink

Candidate: c-parser-shrink. Base: 89a71ae72c721206dfbb63acfeffc74b38dde604.

The complete isolated candidate from vm/cloud-c-parser-shrink was applied to this base with Git three-way application. Host focused O2 and AddressSanitizer/UndefinedBehaviorSanitizer tests were rerun and passed on 2026-10-01. Source changes from upstream are preserved. This is an independent candidate, not a combined integration. Earlier reports describe their original baseline; only the tests stated here were rerun.

LeakSanitizer was disabled because of the environment restriction. ESP-IDF build, hardware, runtime heap, FPS, and device timing are not verified. No COM3 access. No main/vm/main merge.

F precompile remains opt-in build-only; runtime loading is unchanged. A retains the documented bound/proxy limitation and does not fix the separately documented baseline lazy-initialization OOM issue.

## E array trim

Candidate: e-array-trim. Base: 89a71ae72c721206dfbb63acfeffc74b38dde604.

The complete isolated candidate from vm/cloud-e-array-trim was applied to this base with Git three-way application. Host focused O2 and AddressSanitizer/UndefinedBehaviorSanitizer tests were rerun and passed on 2026-10-01. Source changes from upstream are preserved. This is an independent candidate, not a combined integration. Earlier reports describe their original baseline; only the tests stated here were rerun.

LeakSanitizer was disabled because of the environment restriction. ESP-IDF build, hardware, runtime heap, FPS, and device timing are not verified. No COM3 access. No main/vm/main merge.

F precompile remains opt-in build-only; runtime loading is unchanged. A retains the documented bound/proxy limitation and does not fix the separately documented baseline lazy-initialization OOM issue.

## F precompile

Candidate: f-precompile. Base: 89a71ae72c721206dfbb63acfeffc74b38dde604.

The complete isolated candidate from vm/cloud-f-precompile was applied to this base with Git three-way application. Host focused O2 and AddressSanitizer/UndefinedBehaviorSanitizer tests were rerun and passed on 2026-10-01. Source changes from upstream are preserved. This is an independent candidate, not a combined integration. Earlier reports describe their original baseline; only the tests stated here were rerun.

LeakSanitizer was disabled because of the environment restriction. ESP-IDF build, hardware, runtime heap, FPS, and device timing are not verified. No COM3 access. No main/vm/main merge.

F precompile remains opt-in build-only; runtime loading is unchanged. A retains the documented bound/proxy limitation and does not fix the separately documented baseline lazy-initialization OOM issue.

## H typed put

Typed-store candidate based on 89a71ae72c721206dfbb63acfeffc74b38dde604. Full candidate reapplied in isolated worktree. O2 and ASan/UBSan test runners passed, including OFF/ON focused and corpus checks. Default remains OFF. LSan disabled due environment restriction. ESP-IDF build, device speed/heap and integration with other candidates unverified. Prior report refers to old baseline; this note records new-base rerun.
