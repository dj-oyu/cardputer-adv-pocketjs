"""Synthetic runtime-resource contract tests; no hardware or admission."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT/'tools/games'), str(ROOT/'tools')]
from derby_runtime_resources import FLOORS, parse_resources
from test_derby_fixed_completion import receipts
from verify_derby_recorded_trial import verify_recorded_trial

RESOURCE = (b'DERBY_RESOURCE v=1 session=1 reps=3 phase=post_trial_tail valid=1 task=ui unit=bytes'
            b' source=esp-idf-6.0.1 stack_scope=task_lifetime heap_scope=current'
            b' stack_free=8192 internal_free=100000 internal_largest=80000 guest_used=100000 guest_cap=163840\n')
FLOOR = dict(zip(FLOORS, (8192, 100000, 80000, 63840)))


def capture(record=RESOURCE):
    fixture = (ROOT/'tools/games/fixtures/derby_fixed_v1.log').read_bytes()
    return (b'APP_ID local.derby\nDERBY READY tones=true\n' +
            fixture.replace(b'DERBY_FIXED_END', record+b'DERBY_FIXED_END', 1) +
            b'DERBY_DETAIL_EXIT v=1 session=1 clean=1\nAPP_STOPPED\nHOME_READY\n')


class Resources(unittest.TestCase):
    def parse(self, raw, floors=FLOOR, required=True):
        return parse_resources(raw, session='1', required=required, floors=floors)

    def test_threshold_boundaries(self):
        self.assertTrue(self.parse(capture())['resource_verified'])
        for key in FLOORS:
            with self.subTest(key=key):
                floors = dict(FLOOR); floors[key] += 1
                with self.assertRaisesRegex(ValueError, 'below_floor'):
                    self.parse(capture(), floors)
                floors[key] -= 2
                self.assertTrue(self.parse(capture(), floors)['resource_verified'])

    def test_missing_or_incomplete_policy_is_closed(self):
        with self.assertRaisesRegex(ValueError, 'missing'):
            self.parse(capture(b''))
        for bad in (None, {}, {**FLOOR, 'extra': 1}, {**FLOOR, FLOORS[0]: None},
                    {**FLOOR, FLOORS[0]: True}, {**FLOOR, FLOORS[0]: 0},
                    {**FLOOR, FLOORS[0]: -1}, {**FLOOR, FLOORS[0]: 2**32}):
            with self.subTest(policy=bad), self.assertRaisesRegex(ValueError, 'explicit_floors'):
                self.parse(capture(), bad)
        self.assertEqual(self.parse(capture(b''), None, False),
                         dict(available=False, resource_verified=False))
        self.assertFalse(self.parse(capture(), None, False)['resource_verified'])

    def test_schema_scope_identity_and_numeric_negatives(self):
        mutations = [(b'v=1', b'v=2'), (b'session=1', b'session=2'), (b'reps=3', b'reps=2'),
                     (b'task=ui', b'task=audio'), (b'unit=bytes', b'unit=words'),
                     (b'phase=post_trial_tail', b'phase=timed'), (b'valid=1', b'valid=0'),
                     (b'stack_scope=task_lifetime', b'stack_scope=rep_peak'),
                     (b'heap_scope=current', b'heap_scope=lifetime_min'),
                     (b'stack_free=8192', b'stack_free=32769'),
                     (b'internal_largest=80000', b'internal_largest=100001'),
                     (b'guest_cap=163840', b'guest_cap=99999'),
                     (b'source=esp-idf-6.0.1', b'source=unknown')]
        for bad in (b'0', b'-1', b'null', b'true', b'01', b'4294967296', b'999999999999999999'):
            mutations.append((b'stack_free=8192', b'stack_free='+bad))
        for old, new in mutations:
            with self.subTest(new=new), self.assertRaises(ValueError):
                self.parse(capture(RESOURCE.replace(old, new)))
        for bad in (RESOURCE+RESOURCE, RESOURCE[:-1]+b' extra=1\n',
                    RESOURCE[:-1]+b' unit=bytes\n', b'prefix '+RESOURCE,
                    RESOURCE.replace(b' valid=1', b''), b'DERBY_RESOURCE nonsense\n'):
            with self.subTest(record=bad), self.assertRaises(ValueError):
                self.parse(capture(bad), None, False)

    def test_synthetic_order_partial_and_capacity(self):
        synthetic = capture(RESOURCE.replace(b'esp-idf-6.0.1', b'synthetic'))
        self.assertFalse(self.parse(synthetic, None, False)['resource_verified'])
        with self.assertRaisesRegex(ValueError, 'synthetic_not_qualifying'):
            self.parse(synthetic)
        original = capture(b'')
        for bad in (RESOURCE+original, original+RESOURCE, capture()[:-1], b'x'*65537,
                    original.replace(b'DERBY_FIXED_END', b'DERBY_RESOURCE extra\nDERBY_FIXED_END')):
            with self.subTest(raw=bad[:80]), self.assertRaises(ValueError):
                self.parse(bad)

    def test_bound_recorded_chain_and_legacy(self):
        raw = capture()
        result = verify_recorded_trial(*receipts(raw, 'OFF'), expected_head='a'*40,
                                       expected_detail='OFF', require_runtime_resources=True,
                                       resource_floors=FLOOR)
        self.assertTrue(result['runtime_resources']['resource_verified'])
        self.assertFalse(result['hardware_tested'])
        args = receipts(capture(b''), 'OFF')
        self.assertFalse(verify_recorded_trial(*args, expected_head='a'*40,
                         expected_detail='OFF')['runtime_resources']['resource_verified'])
        with self.assertRaisesRegex(ValueError, 'missing'):
            verify_recorded_trial(*args, expected_head='a'*40, expected_detail='OFF',
                                  require_runtime_resources=True, resource_floors=FLOOR)

    def test_device_adapter_task_binding_and_byte_units(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp); (out/'freertos').mkdir()
            headers = {'app_session.h': '#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\nbool app_derby_fixed_guest_memory(size_t*,size_t*);\nbool app_derby_fixed_deadline_ok(uint32_t);\n',
                       'esp_timer.h': '#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
                       'usb_console.h': '#include <stddef.h>\nint usb_console_frame(const char*,size_t);\n',
                       'esp_heap_caps.h': '#include <stddef.h>\ntypedef struct{size_t total_free_bytes,largest_free_block;}multi_heap_info_t;\n#define MALLOC_CAP_INTERNAL 1\n#define MALLOC_CAP_8BIT 2\nvoid heap_caps_get_info(multi_heap_info_t*,unsigned);\n',
                       'freertos/FreeRTOS.h': '#include <assert.h>\n#define configASSERT assert\n',
                       'freertos/task.h': 'typedef void *TaskHandle_t;\nTaskHandle_t xTaskGetCurrentTaskHandle(void);\nconst char *pcTaskGetName(TaskHandle_t);\nunsigned uxTaskGetStackHighWaterMark(TaskHandle_t);\n'}
            for name, text in headers.items():
                (out/name).write_text(text)
            source = out/'test.c'
            source.write_text(r'''
#include "derby_fixed_workload.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include <assert.h>
static TaskHandle_t current=(void*)1;static const char *name="ui";
static unsigned phase=DF_TAIL,queries;static bool guest_ok=true;
TaskHandle_t xTaskGetCurrentTaskHandle(void){return current;}
const char *pcTaskGetName(TaskHandle_t t){assert(t==(void*)1);return name;}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t t){assert(t==(void*)1);return 512;}
unsigned df_status(void){return phase;}
bool app_derby_fixed_guest_memory(size_t *u,size_t *c){*u=100;*c=200;return guest_ok;}
bool app_derby_fixed_deadline_ok(uint32_t reserve){return reserve==125000;}
void heap_caps_get_info(multi_heap_info_t *i,unsigned caps){assert(caps==3);++queries;i->total_free_bytes=400;i->largest_free_block=300;}
int64_t esp_timer_get_time(void){return 1;}
int usb_console_frame(const char *p,size_t n){(void)p;(void)n;return 0;}
int main(void){df_resources r;
 assert(derby_runtime_resources_deadline_ok(125000)&&!derby_runtime_resources_deadline_ok(0));
 assert(!derby_runtime_resources_read(&r)&&!queries);derby_runtime_resources_bind_ui_task();
 assert(derby_runtime_resources_read(&r)&&queries==1&&r.ui_stack_free==512&&!r.synthetic);
 assert(r.guest_used==100&&r.guest_cap==200&&r.internal_free==400&&r.internal_largest==300);
 current=(void*)2;assert(!derby_runtime_resources_read(&r));current=(void*)1;
 name="audio";assert(!derby_runtime_resources_read(&r));name="ui";
 phase=DF_MEASURE;assert(!derby_runtime_resources_read(&r));phase=DF_TAIL;
 guest_ok=false;assert(!derby_runtime_resources_read(&r));assert(!derby_runtime_resources_read(0)&&queries==1);
}
''')
            binary = out/'test'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-DPOCKET_DERBY_FIXED_WORKLOAD', '-I', str(out), '-I', str(ROOT/'main/pocket'),
                            str(source), str(ROOT/'main/pocket/derby_fixed_workload_device.c'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
