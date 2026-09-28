#include "ksn_grid_scan_cost_device_probe.h"

#include "ksn_proc_grid_pie.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>
#include <string.h>

#define TAG "KSN_GRID_SCAN"
#define MAX_WIDTH 64u
#define MAX_HEIGHT 17u
#define PITCH (MAX_WIDTH + 1u)
#define CALLS_PER_ARM 512u

static int16_t source[MAX_WIDTH * MAX_HEIGHT] __attribute__((aligned(16)));
static int16_t scalar[PITCH * MAX_HEIGHT] __attribute__((aligned(16)));
static int16_t pie[PITCH * MAX_HEIGHT] __attribute__((aligned(16)));
static ksn_grid_program program;
static ksn_grid_plan plan;
static ksn_grid_execution execution[2];
static volatile uint32_t checksum;

static ksn_grid_index index5(int32_t base, int32_t x, int32_t y)
{
    ksn_grid_index index = {0};
    int32_t terms[5] = {base, x, y, 0, 0};
    for (unsigned i = 0; i < 5; ++i) {
        index.term[i].constant = terms[i];
        index.term[i].param = KSN_GRID_NO_PARAM;
    }
    return index;
}

static void make_program(ksn_grid_program *p, bool iir)
{
    memset(p, 0, sizeof *p);
    p->count = iir ? 6 : 4;
    p->final_shift = iir ? 1 : 0;
    p->body[0].op = KSN_GRID_LOAD;
    p->body[0].dst = 1;
    p->body[0].buffer = KSN_GRID_DEST;
    p->body[0].index = index5(0, 1, PITCH);
    unsigned slot = 1;
    if (iir) {
        p->body[slot].op = KSN_GRID_CONST;
        p->body[slot].dst = 2;
        p->body[slot++].immediate = 2;
        p->body[slot].op = KSN_GRID_MUL;
        p->body[slot].dst = 3;
        p->body[slot].a = 1;
        p->body[slot++].b = 2;
    }
    p->body[slot].op = KSN_GRID_LOAD;
    p->body[slot].dst = 4;
    p->body[slot].buffer = KSN_GRID_SOURCE;
    p->body[slot++].index = index5(0, 1, MAX_WIDTH);
    p->body[slot].op = KSN_GRID_ADD;
    p->body[slot].dst = 5;
    p->body[slot].a = iir ? 3 : 1;
    p->body[slot++].b = 4;
    p->body[slot].op = KSN_GRID_ADD;
    p->body[slot].dst = 0;
    p->body[slot].a = 0;
    p->body[slot].b = 5;
    p->output = index5(1, 1, PITCH);
}

static bool run_case(bool iir, unsigned width, unsigned height)
{
    ksn_grid_binding binding = {0};
    ksn_grid_shape shape = {(uint16_t)width, (uint16_t)height, 1, 1};
    int16_t *out[2] = {scalar, pie};
    int64_t elapsed[2] = {0, 0};
    make_program(&program, iir);
    if (ksn_grid_prepare(&program, &plan) != KSN_GRID_OK) return false;
    for (unsigned y = 0; y < height; ++y) {
        scalar[y * PITCH] = pie[y * PITCH] = y & 1 ? -512 : 512;
        for (unsigned x = 0; x < width; ++x)
            source[y * MAX_WIDTH + x] =
                (int16_t)(((x * 37u + y * 19u) % 127u) - 63);
    }
    binding.data[KSN_GRID_SOURCE] = source;
    binding.count[KSN_GRID_SOURCE] = MAX_WIDTH * MAX_HEIGHT;
    binding.count[KSN_GRID_DEST] = PITCH * MAX_HEIGHT;
    for (unsigned arm = 0; arm < 2; ++arm) {
        binding.data[KSN_GRID_DEST] = out[arm];
        if (ksn_grid_begin(&plan, &shape, &binding, &execution[arm]) !=
                KSN_GRID_OK || !ksn_grid_scan_pie_eligible(&execution[arm]) ||
            execution[arm].scan_prev_coefficient != (iir ? 2 : 1))
            return false;
    }
    /* Four alternating 128-call windows. Bind and the first warm call are
     * outside timing. Seed columns remain fixed between calls. */
    for (unsigned trial = 0; trial < 4; ++trial) {
        for (unsigned order = 0; order < 2; ++order) {
            unsigned arm = (trial + order) & 1u;
            ksn_grid_status (*run)(ksn_grid_execution *) =
                arm ? ksn_grid_run_scan_pie : NULL;
            if ((run ? run(&execution[arm]) :
                       ksn_grid_run_scalar(&execution[arm])) != KSN_GRID_OK)
                return false;
            int64_t start = esp_timer_get_time();
            for (unsigned call = 0; call < CALLS_PER_ARM / 4u; ++call)
                if ((run ? run(&execution[arm]) :
                           ksn_grid_run_scalar(&execution[arm])) != KSN_GRID_OK)
                    return false;
            elapsed[arm] += esp_timer_get_time() - start;
            checksum += (uint16_t)out[arm][PITCH + width];
        }
        vTaskDelay(1);
    }
    for (unsigned y = 0; y < height; ++y)
        if (memcmp(scalar + y * PITCH, pie + y * PITCH,
                   (width + 1u) * sizeof(int16_t))) return false;
    ESP_LOGI(TAG, "GRID_SCAN_TIME kind=%s width=%u height=%u calls_per_backend=%u scalar_us=%lld pie_us=%lld checksum=%lu",
             iir ? "iir" : "prefix", width, height, CALLS_PER_ARM,
             (long long)elapsed[0], (long long)elapsed[1],
             (unsigned long)checksum);
    return true;
}

bool ksn_grid_scan_cost_device_probe_run(void)
{
    static const unsigned sizes[][2] = {{8, 8}, {32, 16}, {64, 17}};
    if (!ksn_grid_pie_backend_available()) return false;
    for (unsigned kind = 0; kind < 2; ++kind)
        for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; ++i)
            if (!run_case(kind != 0, sizes[i][0], sizes[i][1])) {
                ESP_LOGE(TAG, "GRID_SCAN FAIL kind=%s width=%u height=%u",
                         kind ? "iir" : "prefix", sizes[i][0], sizes[i][1]);
                return false;
            }
    ESP_LOGI(TAG, "GRID_SCAN PASS");
    return true;
}
