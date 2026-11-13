# ser() の整数化・float32 の試作（host）

[docs/apps/derby-ser-division.md](../../../docs/apps/derby-ser-division.md) の設計で使った host の試作（S0 で `tools/games/` の正式な検査へ育てる前の写し）。

- `serq.c` — 整数（位置 Q16・向き Q30・64 ビットの積）の `ser()` と、倍精度の写しとの画面上の点の照合。
- `serq_v.c`・`serq_x.c` — 整数版の変種（検証用・Xtensa で組んで命令数を見る用）。
- `serf.c` — float32 版（代替案 (b) の誤差の測定）。
- `serc.c` — 倍精度の `ser()` の除算・sqrt などの回数を数える版。

各ファイルの先頭のコメントに組み方がある。ここの数字は設計書の §（実測）に記録した。

段 S0 で正式な検査 `tools/games/test_derby_serq.c`（ファームの `main/pocket/derby_ser_int.c` そのものを倍精度と照合し、`run_derby.py` から回る）になった。ここの試作は設計の数字の出所として残す。
