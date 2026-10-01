# Cloud validation on 89a71ae

Candidate: e-array-trim. Base: 89a71ae72c721206dfbb63acfeffc74b38dde604.

The complete isolated candidate from vm/cloud-e-array-trim was applied to this base with Git three-way application. Host focused O2 and AddressSanitizer/UndefinedBehaviorSanitizer tests were rerun and passed on 2026-10-01. Source changes from upstream are preserved. This is an independent candidate, not a combined integration. Earlier reports describe their original baseline; only the tests stated here were rerun.

LeakSanitizer was disabled because of the environment restriction. ESP-IDF build, hardware, runtime heap, FPS, and device timing are not verified. No COM3 access. No main/vm/main merge.

F precompile remains opt-in build-only; runtime loading is unchanged. A retains the documented bound/proxy limitation and does not fix the separately documented baseline lazy-initialization OOM issue.
