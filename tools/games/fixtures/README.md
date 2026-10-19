`pr47_derby_fixed.py` is an unchanged snapshot of
[`src/cardputer_mcp/derby_fixed.py`](https://github.com/dj-oyu/esp-device-broker-mcp/blob/4f31347859824300bd71f1232f56c450d817ef4e/src/cardputer_mcp/derby_fixed.py)
from the deployed PR47 head. SHA-256:
`901bec0c05333be0314f19a0a8d2231397745205414ef10eb9de4309436838c6`.

`run_derby_fixed.py` runs the actual generated fixture in QuickJS and native C,
then passes that output through this collector and its `execute_fixed` entry
point. The host models APP_ID and stop/home after cleanup; it does not open a
port or establish device lifecycle acceptance. Startup READY is never supplied
by the transport model. Both tone capability states and failed initialization
are checked with the actual generated app. The pinned collector keeps its
20-second startup wait, 200-second capture limit and byte/quality guards.

The source heads changed because the former quiet logger suppressed the app's
post-initialization READY, causing `measure-derby-pr13-e93500d-20261019` to time
out. The fix only passes that existing marker; rep resets remain quiet. Old
source/build/flash receipts and failed raw bytes remain evidence for the old
heads and must not be relabelled as evidence for these new heads.
