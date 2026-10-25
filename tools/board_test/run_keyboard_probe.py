"""Compile the actual startup helper against deterministic SDK fakes; no hardware."""
from pathlib import Path
import argparse,os,subprocess
R=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');a=p.parse_args()
out=R/'.cache'/('keyboard-probe-sanitize' if a.sanitize else 'keyboard-probe')
stubs={
 'driver/i2c_master.h':'#pragma once\n#include <stdint.h>\n#include "esp_err.h"\ntypedef void *i2c_master_bus_handle_t;\nesp_err_t i2c_master_probe(i2c_master_bus_handle_t,uint16_t,int);\n',
 'driver/gpio.h':'#pragma once\nint gpio_get_level(int);\n',
 'freertos/FreeRTOS.h':'#pragma once\n#include <stdint.h>\ntypedef uint32_t TickType_t;\n#define pdMS_TO_TICKS(ms) ((TickType_t)((ms)*configTICK_RATE_HZ/1000))\n',
 'freertos/task.h':'#pragma once\n#include "FreeRTOS.h"\nvoid vTaskDelay(TickType_t);\n',
 'esp_log.h':'#pragma once\nvoid test_log(char,const char*,const char*,...);\n#define ESP_LOGI(t,...) test_log(\'I\',t,__VA_ARGS__)\n#define ESP_LOGW(t,...) test_log(\'W\',t,__VA_ARGS__)\n#define ESP_LOGE(t,...) test_log(\'E\',t,__VA_ARGS__)\n',
}
for name,text in stubs.items():
 q=out/'include'/name;q.parent.mkdir(parents=True,exist_ok=True);q.write_text(text)
board=(R/'main/hal/board.c').read_text()
start=board.index('    esp_err_t keyboard_error = keyboard_probe_startup(ih);')
end=board.index('    battery_init();',start)
(out/'include/keyboard_stage.inc').write_text(board[start:end])
for hz in [100,1000]:
 binary=out/f'keyboard-probe-{hz}'
 flags=['-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie'] if a.sanitize else ['-O2']
 subprocess.run([os.environ.get('CC','gcc'),'-std=c11','-Wall','-Wextra','-Werror',*flags,
     f'-DconfigTICK_RATE_HZ={hz}','-I'+str(out/'include'),'-Itools/hostshim','-Imain/hal',
     'tools/board_test/test_keyboard_probe.c','main/hal/keyboard_probe.c','-o',str(binary)],cwd=R,check=True)
 subprocess.run([str(binary)],cwd=R,check=True)
