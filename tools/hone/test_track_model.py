"""Exercise compiled track admission, geometry, color, coverage and pin lifetime."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
ASSET = r"""
#define HONE_TRACK_VERTICES 7
#define HONE_TRACK_TRIANGLES 4
#define HONE_TRACK_CHANNELS 2
#define HONE_TRACK_COLORS 2
#define HONE_TRACK_KEYS 2
#define HONE_TRACK_GROUPS 2
static const hone_track_vertex track_vertices[] = {
    {{-16,16,8},{1,0,0}}, {{80,16,8},{1,0,0}}, {{80,80,8},{1,0,0}},
    {{-16,80,8},{1,0,0}}, {{0,32,20},{0}}, {{48,32,20},{0}}, {{0,80,20},{0}}
};
static const hone_track_triangle track_triangles[] = {
    {{2,1,0},0xf800,0,1}, {{0,2,3},0xf800,0,1},
    {{4,5,6},0x07e0,1,0}, {{0,1,2},0xffff,0,0}
};
static const int8_t track_delta[][2] = {{0,0},{0,4}};
static const uint16_t track_scale[] = {0,16};
static const uint16_t track_color[][2] = {{0,0},{0xf800,0x001f}};
static const uint8_t track_visible[][1] = {{3},{1}};
static const hone_track_pose track_poses[] = {{0,1,0},{0,1,128},{0,1,255}};
"""
CHECK = r"""
#include "ayatsuri/ayatsuri_live_model_fixture_api.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    uint16_t band[8*240], row[240];
    for (unsigned pose=0;pose<3;++pose) {
        unsigned packets;bool fast;uint32_t us;
        assert(ay_live_model_prepare(pose,&packets,&fast,&us) && packets && fast);
        assert(!ay_live_model_prepare(pose,&packets,&fast,&us) && !ay_live_model_close());
        assert(!ay_live_model_finish(false));
        int offset=2*(int)pose;
        uint16_t color=pose==0?0xf800:pose==1?0x7810:0x001f;
        for (unsigned top=0;top<135;top+=8) {
            unsigned left,right;
            memset(band,0xdd,sizeof band);
            assert(ay_live_model_band(top,band,&left,&right));
            unsigned n=top==128?7:8;
            for (unsigned j=0;j<n;++j) {
                assert(ay_live_model_row(top+j,row));
                assert(!memcmp(band+240*j,row,sizeof row));
                for (unsigned x=0;x<240;++x) {
                    int y=top+j;
                    bool square=(int)x>=offset-1 && (int)x<offset+5 && y>=1 && y<5;
                    bool overlay=!pose && y>=2 && (int)x+y<5;
                    uint16_t expected=overlay?0x07e0:square?color:0;
                    assert(row[x]==expected);
                    if (row[x]) assert(x>=left && x<right);
                }
            }
            if(top==128)assert(band[7*240]==0xdddd);
        }
        assert(ay_live_model_finish(true));
        assert(!ay_live_model_row(0,row));
    }
    assert(ay_live_model_close());
    puts("PASS track key/midframe visibility, RGB565, depth, winding, clipping, band extent and DMA pin");
    return 0;
}
"""
def main():
    with tempfile.TemporaryDirectory() as tmp:
        temp = Path(tmp)
        config = temp / 'companion_motion_config.h'
        config.write_text('#define AY_COMPANION_MOTION_POSES 3u\n#define AY_LIVE_MODEL_SHA256 "test"\n')
        (temp / 'hone_track_asset.h').write_text(ASSET)
        (temp / 'check.c').write_text(CHECK)
        exe = temp / ('check.exe' if os.name == 'nt' else 'check')
        subprocess.run([*shlex.split(os.environ.get('CC','cc')), '-std=c11', '-O2', '-Wall', '-Wextra',
                        '-Werror', '-ffp-contract=off', '-DAYATSURI_TRACK_MODEL=1', '-I', str(temp),
                        '-I', str(ROOT/'main'), str(ROOT/'main/hone/track_model.c'), str(temp/'check.c'),
                        '-o', str(exe)], check=True)
        subprocess.run([exe], check=True)

if __name__ == '__main__':
    main()
