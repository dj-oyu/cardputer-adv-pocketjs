# Native mesh foundation adoption

This review layer starts at main `b2cac9989cf67979a578b583a73d555226abfe6e`
through the existing integration commit `724d97ad` (16 commits, 117 changed files
before this CI/documentation addition). It is a framework change, not a four-file
raster optimization.

The layer contains owned native mesh packets and frame/depth lifecycle, managed
model and scene APIs, detached skin evaluation and its render/JS/batch bridge,
an opt-in geometry provider, a public synthetic MESH BENCH app, the shared native
class registry, and an optional retained triangle-setup backend. Existing main
Shiki and LCD changes are preserved by ancestry.

Prepared setup and the S3 edge4 experiment remain default OFF. The provider is
explicitly macro-gated. There are no sdkconfig or security-setting changes in
this layer. The new MESH BENCH menu entry and common class registry are intentional
product-surface changes and must be reviewed with lifecycle regressions.

The separate Q7 review layer depends on this foundation. Neither its certified
renderer nor its four-file flat-span optimization can be applied directly to main:
main lacks the owned mesh frame, compact packet, depth, and prepared interfaces.
Do not merge or enable Q7 by assuming that the foundation's acceptance implies
acceptance of approximate coverage/color.

## Validation

`native-mesh.yml` checks the exact PR head, not GitHub's synthetic merge commit.
It runs native mesh/depth/memory, class registry, scene, skin and provider,
prepared unit/lifecycle, Shiki API and MEGADEMO host regressions. A separate
ESP-IDF 6.0.1 build checks default-off flags and absence of the Q7 renderer.
Logs are named by source SHA. CI outcomes must be read on the actual published
head before adoption; adding a workflow is not a passing result.

Synthetic host checks do not establish private-model image quality, device
performance, or full framework completion. This layer requests review only,
not device writes or a main merge.
