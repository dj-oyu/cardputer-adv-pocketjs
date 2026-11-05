#!/usr/bin/env python3
"""Host test for tools/frame_lint.py's scanner, on canned objdump text (no toolchain)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import frame_lint

TEXT = """
00000000 <far_fn>:
   0:	entry	a1, 0x4d0
   3:	movi	a9, 0x424
   6:	add.n	a9, a9, a1
   8:	l32i.n	a8, a9, 0
   a:	s32i.n	a8, a9, 4
   c:	addmi	a10, a1, 0x400
   f:	s32i.n	a8, a10, 8
  11:	movi	a11, 0x430
  14:	add	a11, a1, a11
  17:	addi	a11, a11, 4
  1a:	l16ui	a8, a11, 2
  1d:	l32i	a8, a1, 0x3fc
  20:	movi	a12, 0x500
  23:	beqz	a8, 2c <far_fn+0x2c>
  26:	add.n	a12, a12, a1
  28:	l32i.n	a8, a12, 0
  2a:	j	30 <far_fn+0x30>
  2c:	l32i.n	a8, a12, 0
  2e:	movi	a13, 0x404
  31:	add.n	a13, a13, a1
  33:	movi.n	a13, 0
  35:	l32i.n	a8, a13, 0
  37:	retw.n

00000040 <big_frame_no_far>:
  40:	entry	a1, 0xaa0
  43:	l32i	a8, a1, 16
  46:	retw.n

00000050 <small>:
  50:	entry	a1, 64
  53:	movi	a9, 0x424
  56:	add.n	a9, a9, a2
  58:	l32i.n	a8, a9, 0
  5a:	retw.n
"""

rows = {r.func: r for r in frame_lint.scan(TEXT.splitlines(), "canned")}
far = rows["far_fn"]
# movi+add (2 accesses), addmi (1), add a1-first then addi (1, deepest 0x430+4+2),
# the access after the branch target 0x2c is not credited, and a13 is
# overwritten before its use.
assert far.frame == 0x4d0, far
assert far.accesses == 5, far          # 0x424 x2, 0x400, 0x436, 0x500 (before the label)
assert far.computes == 5, far          # 0x424, 0x400, 0x430, 0x500, 0x404
assert far.extra == 2 + 1 + 2 + 2 + 2, far
assert far.worst == 0x500, far
assert rows["big_frame_no_far"].accesses == 0 and rows["big_frame_no_far"].frame == 0xaa0
assert "small" not in rows             # a2-relative, not the stack
print("FRAME_LINT_TEST PASS")
