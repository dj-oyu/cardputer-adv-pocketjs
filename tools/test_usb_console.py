"""Actual console/bridge writer and HOME teardown; no SDK execution or USB.

--idf also compiles the actual IDF 6.0.1 VFS functions to reproduce a lost LF
which is nevertheless counted as accepted. The normal test needs only GCC.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(idf=None):
    with tempfile.TemporaryDirectory(prefix="usb-console-") as temporary:
        out = Path(temporary)
        headers = {
            "esp_err.h": "#pragma once\ntypedef int esp_err_t;\nenum {ESP_OK=0,ESP_FAIL=-1,ESP_ERR_INVALID_STATE=1,ESP_ERR_NO_MEM,ESP_ERR_INVALID_SIZE,ESP_ERR_TIMEOUT};\n",
            "esp_timer.h": "#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
            "freertos/FreeRTOS.h": "#pragma once\n#include <stdint.h>\ntypedef uint32_t TickType_t;\n#define portTICK_PERIOD_MS 1\n#define pdTRUE 1\n",
            "freertos/semphr.h": "#pragma once\n#include \"FreeRTOS.h\"\ntypedef struct {int held;} StaticSemaphore_t;\ntypedef StaticSemaphore_t *SemaphoreHandle_t;\nSemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *);\nint xSemaphoreTake(SemaphoreHandle_t,TickType_t);\nvoid xSemaphoreGive(SemaphoreHandle_t);\n",
            "driver/usb_serial_jtag.h": "#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\nint usb_serial_jtag_write_bytes(const void *,size_t,uint32_t);\nbool usb_serial_jtag_is_driver_installed(void);\n",
            "esp_vfs.h": "#pragma once\n#include <sys/types.h>\n#include <sys/stat.h>\n#include \"esp_err.h\"\n#define ESP_VFS_FLAG_DEFAULT 0\ntypedef struct {int flags;ssize_t (*write)(int,const void*,size_t);int (*open)(const char*,int,int);int (*close)(int);int (*fstat)(int,struct stat*);} esp_vfs_t;\nesp_err_t esp_vfs_register(const char*,const esp_vfs_t*,void*);\n",
        }
        for name, text in headers.items():
            path = out / name
            path.parent.mkdir(exist_ok=True, parents=True)
            path.write_text(text)
        main = (ROOT / "main/main.c").read_text()
        start = main.index("static void end_run(esp_err_t tick_err) {")
        end = main.index("// Resident suspension", start)
        (out / "end_run.inc").write_text(main[start:end])
        # The transport, rather than altered navigation/marker semantics, is
        # the repair. Protect the existing q/Esc and shared HOME teardown path.
        assert "else if(c=='q'||c==27)k->nav=KEY_BACK;" in main
        assert main.index("usb_console_init()") < main.index('ESP_LOGI("boot","Cardputer')
        bridge = (ROOT / "main/pocket/pocket_bridge.c").read_text()
        assert "return usb_console_frame(wire,at);" in bridge
        assert "usb_serial_jtag_write_bytes" not in bridge
        for flags in (["-O1", "-g", "-fsanitize=address,undefined"], ["-O2", "-fstrict-aliasing"]):
            exe = out / "console-test"
            subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", *flags,
                            "-Dfreopen=host_freopen", "-Dsetvbuf=host_setvbuf",
                            "-I", str(out), "-I", str(ROOT / "main/hal"),
                            str(ROOT / "main/hal/usb_console.c"),
                            str(ROOT / "tools/test_usb_console.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1",
                                    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
        if idf:
            source = Path(idf) / "components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c"
            text = source.read_text()
            assert "#define TX_FLUSH_TIMEOUT_US (50*1000LL)" in text
            tx = text[text.index("static void usb_serial_jtag_tx_char_no_driver(int fd, int c)\n{"):]
            tx = tx[:tx.index("static int usb_serial_jtag_rx_char_no_driver")]
            write = text[text.index("static ssize_t usb_serial_jtag_write("):]
            write = write[:write.index("/* Helper function")]
            (out / "idf_vfs.inc").write_text(tx + write)
            exe = out / "idf-vfs-test"
            subprocess.run(["gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-O2", "-I", str(out),
                            str(ROOT / "tools/test_usb_vfs_loss.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", help="checkout of ESP-IDF v6.0.1 (source inspection only)")
    run(parser.parse_args().idf)
