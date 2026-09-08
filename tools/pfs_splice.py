"""Write a variant flower_shots.h: the live header with its table replaced.

Used only by tools/preview_flower_shots.sh. The body arrives in $PFS_BODY so it
survives the shell's heredocs; the destination path is argv[1]. Everything
outside the table -- the contract, the pitch cap, the constraints -- is carried
over verbatim, because a variant is a different set of shots, not a different
renderer.
"""
import os
import sys
import pathlib

NL = chr(10)
body = os.environ['PFS_BODY'].rstrip(NL) + NL
src = pathlib.Path('main/scene/flower_shots.h').read_text()
head = src[:src.index('static const flower_shot_t FLOWER_SHOTS[]={')]
tail = src[src.index('#define FLOWER_VIEWS'):]

count = body.count('{')
assert count >= 1, 'variant table has no entries'
# The shortest dwell has to keep matching the table, or test_flower.c's bound on
# the dissolving fraction is measured against a number the table disagrees with.
tail = tail.replace('#define FLOWER_VIEW_MIN_S 40.0f',
                    '#define FLOWER_VIEW_MIN_S %.1ff' % (40.0 / count))

pathlib.Path(sys.argv[1]).write_text(
    head + 'static const flower_shot_t FLOWER_SHOTS[]={' + NL + body + '};' + NL + tail)
