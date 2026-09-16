"""Execute the real continuation/reset function bodies with a fake clock.

This isolates cadence/publication, not the ESP allocator or FreeRTOS HWM.
Run in WSL with gcc; no firmware or connected device is modified.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

from vm_l0_capture import Collector

SOURCE = Path(__file__).resolve().parents[1] / 'main/pocket/vmprobe.c'


def function(source, name):
    start = source.index('void ' + name + '(')
    opening = source.index('{', start)
    depth = 1
    pos = opening + 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]


class ContinuationTest(unittest.TestCase):
    def test_real_cadence_and_reset(self):
        source = SOURCE.read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef int pocketjs_guest_t;
#define VMPROBE_SAMPLE_EVERY_N_FRAMES 8
#define VMPROBE_WINDOW_US 1000000
static unsigned continuation_ticks, samples, flushes, published, continuation_heap_n;
static int64_t now, window_start_us;
static uint64_t jobs_executed_base;
static int64_t esp_timer_get_time(void) { return now; }
static void heap_sample(pocketjs_guest_t *guest) { assert(guest); samples++; }
static void window_reset(void) { window_start_us = now; samples = 0; continuation_heap_n = 0; }
static void flush_window(void) {
    assert(samples == continuation_heap_n);
    flushes++; published = samples; window_reset();
}
static uint64_t qjs_vmprobe_jobs_executed_get(void) { return 13; }
static unsigned qjs_vmprobe_job_queue_peak_take(void) { return 0; }
static void pocketjs_guest_vmprobe_take(void *a, void *b) { (void)a; (void)b; }
static unsigned pocketjs_guest_vmprobe_drain_calls(void *a, unsigned b, void *c)
{ (void)a; (void)b; (void)c; return 0; }
'''
        code += function(source, 'vmprobe_continuation_sample')
        code += function(source, 'vmprobe_session_reset')
        code += r'''
int main(void) {
    pocketjs_guest_t guest = 1;
    for (int i = 0; i < 7; i++) vmprobe_continuation_sample(&guest);
    assert(samples == 0 && flushes == 0);
    vmprobe_continuation_sample(&guest);
    assert(samples == 1);
    now = VMPROBE_WINDOW_US;
    vmprobe_continuation_sample(&guest);
    assert(flushes == 1 && published == 1 && samples == 0);
    // Publishing a window must not shift the every-eight-turn cadence.
    for (int i = 0; i < 7; i++) vmprobe_continuation_sample(&guest);
    assert(samples == 1 && flushes == 1);
    // A partial session must not shift the next session's first sample.
    vmprobe_continuation_sample(&guest);
    vmprobe_session_reset();
    assert(jobs_executed_base == 13 && continuation_ticks == 0);
    for (int i = 0; i < 7; i++) vmprobe_continuation_sample(&guest);
    assert(samples == 0);
    vmprobe_continuation_sample(&guest);
    assert(samples == 1);
    now += VMPROBE_WINDOW_US;
    vmprobe_continuation_sample(&guest);
    assert(flushes == 2 && published == 1);
}
'''
        with tempfile.TemporaryDirectory(prefix='vmprobe-test-') as temp:
            exe = str(Path(temp) / 'cadence')
            subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-x', 'c', '-', '-o', exe], input=code, text=True, check=True)
            subprocess.run([exe], check=True)

    def test_memory_only_window(self):
        collector = Collector()
        collector.feed('VMPROBE WINDOW seq=0 cond=0 ms=1000 frames=0 lat_n=0 lat_drop=0 '
                       'qpeak_max=0 heap_free_min=100 heap_largest_min=80 js_used_max=20 '
                       'js_limit=100 stack_hw_min=500 flush_us=1 drainrun_drop=0 depth_drop=0')
        collector.close()
        self.assertEqual(collector.bad, 0)
        self.assertEqual(collector.records[0]['samples'], {})
        self.assertEqual(collector.records[0]['js_used_max'], 20)
        self.assertEqual(collector.records[0]['continuation_heap_n'], 0)
        collector.feed('VMPROBE WINDOW seq=1 cond=0 ms=1000 frames=0 lat_n=0 lat_drop=0 '
                       'qpeak_max=0 heap_free_min=100 heap_largest_min=80 js_used_max=20 '
                       'js_limit=100 stack_hw_min=500 flush_us=1 drainrun_drop=0 depth_drop=0 '
                       'frame_heap_n=0 continuation_heap_n=4')
        collector.close()
        self.assertEqual(collector.records[1]['continuation_heap_n'], 4)


if __name__ == '__main__':
    unittest.main()
