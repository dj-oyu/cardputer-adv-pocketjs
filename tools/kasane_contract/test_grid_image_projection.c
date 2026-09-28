#include "core_fixture.h"
#include "ksn_proc_grid_image.h"
#include "ksn_render.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t panel[240 * 135], strip[240 * 8];
static uint16_t *strip_buffer(void *ctx) { (void)ctx; return strip; }
static ksn_result present(void *ctx, uint16_t y, uint16_t rows,
                          const uint16_t *pixels)
{
    (void)ctx;
    memcpy(panel + (size_t)y * 240, pixels, (size_t)rows * 240 * 2);
    return KSN_OK;
}

static ksn_grid_coeff coeff(int32_t value)
{
    return (ksn_grid_coeff){.constant = value, .param = KSN_GRID_NO_PARAM};
}

static ksn_grid_index index5(int32_t a, int32_t x, int32_t y)
{
    ksn_grid_index index = {0};
    index.term[0] = coeff(a);
    index.term[1] = coeff(x);
    index.term[2] = coeff(y);
    index.term[3] = coeff(0);
    index.term[4] = coeff(0);
    return index;
}

int main(void)
{
    _Alignas(16) int16_t source[16 * 8], pixels[16 * 8];
    for (unsigned y = 0; y < 8; ++y)
        for (unsigned x = 0; x < 16; ++x)
            source[y * 16 + x] = (int16_t)(0x001fu + y * 0x0800u + x);
    ksn_grid_program program = {0};
    program.count = 2;
    program.body[0] = (ksn_grid_instruction){.op = KSN_GRID_LOAD,
        .dst = 1, .buffer = KSN_GRID_SOURCE, .index = index5(0, 1, 16)};
    program.body[1] = (ksn_grid_instruction){.op = KSN_GRID_ADD,
        .dst = 0, .a = 0, .b = 1};
    program.output = index5(0, 1, 16);
    ksn_grid_plan plan;
    assert(ksn_grid_prepare(&program, &plan) == KSN_GRID_OK);
    ksn_grid_binding binding = {0};
    binding.data[KSN_GRID_SOURCE] = source;
    binding.count[KSN_GRID_SOURCE] = 16 * 8;
    binding.data[KSN_GRID_DEST] = pixels;
    binding.count[KSN_GRID_DEST] = 16 * 8;
    ksn_grid_shape shape = {16, 8, 1, 1};
    ksn_grid_image grid;
    ksn_grid_pie_policy policy = {true, 8, 0, 0};
    assert(ksn_grid_image_bind(&grid, &plan, &shape, &binding, policy) ==
           KSN_GRID_OK);
    ksn_image_port port;
    assert(!ksn_grid_image_port(&grid, &port));
    assert(ksn_grid_image_run(&grid) == KSN_GRID_OK);
    assert(ksn_grid_image_port(&grid, &port));
    assert(port.opaque && port.width == 16 && port.height == 8);
    uint16_t row[16];
    uint8_t alpha[16];
    assert(port.read_span(port.ctx, 0, 0, 3, 0, 16, row, alpha) == KSN_OK);
    for (unsigned x = 0; x < 16; ++x) {
        assert(row[x] == (uint16_t)source[3 * 16 + x]);
        assert(alpha[x] == 255);
    }
    assert(port.read_span(port.ctx, 0, 0, 8, 0, 1, row, alpha) == KSN_INVALID);

    KSN_TEST_CORE(core,);
    ksn_core_init(&core);
    ksn_resource resource;
    assert(ksn_core_register_image(&core, KSN_APP, &port, &resource) == KSN_OK);
    ksn_client app = ksn_core_client(&core, KSN_APP);
    ksn_tx tx;
    ksn_ref image_ref, rect_ref;
    ksn_draw image = {.kind = KSN_IMAGE, .bounds = {0, 0, 32, 16},
        .clip = {0, 0, 240, 135}, .opacity = 255,
        .data.image = {.resource = resource, .scale = KSN_IMAGE_STRETCH,
                       .source_width = 16, .source_height = 8}};
    ksn_draw rect = {.kind = KSN_RECT, .bounds = {8, 4, 16, 12},
        .clip = {0, 0, 240, 135}, .opacity = 255,
        .data.shape = {.color = 0xff0000ff}};
    assert(app.ops->begin(app.ctx, KSN_REPLACE, &tx) == KSN_OK);
    assert(app.ops->background(app.ctx, tx, 0x000000ff) == KSN_OK);
    assert(app.ops->add(app.ctx, tx, &image, &image_ref) == KSN_OK);
    assert(app.ops->add(app.ctx, tx, &rect, &rect_ref) == KSN_OK);
    assert(app.ops->end(app.ctx, tx) == KSN_OK);
    ksn_display_port display = {NULL, strip_buffer, present, 240, 135, 8,
                                NULL, NULL};
    ksn_render_stats stats;
    assert(ksn_render_rects(&core, &display, &stats) == KSN_OK);
    assert(panel[0] == (uint16_t)source[0]);
    assert(panel[5 * 240 + 10] == 0xf800u);
    assert(panel[15 * 240 + 31] == (uint16_t)source[7 * 16 + 15]);
    assert(panel[20 * 240 + 20] == 0);

    assert(app.ops->begin(app.ctx, KSN_PATCH, &tx) == KSN_OK);
    ksn_change move = {.property = KSN_SET_RECT,
                       .value.rect = {32, 0, 48, 8}};
    assert(app.ops->change(app.ctx, tx, image_ref, &move) == KSN_OK);
    assert(app.ops->end(app.ctx, tx) == KSN_OK);
    assert(ksn_render_rects(&core, &display, &stats) == KSN_OK);
    assert(panel[0] == 0);
    assert(panel[5 * 240 + 10] == 0xf800u);
    assert(panel[7 * 240 + 47] == (uint16_t)source[7 * 16 + 15]);
    puts("grid image projection: native grid, zoom, UI overlay and move passed");
    return 0;
}
