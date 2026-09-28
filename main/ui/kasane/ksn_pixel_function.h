#ifndef KSN_PIXEL_FUNCTION_H
#define KSN_PIXEL_FUNCTION_H

#include "ksn_ports.h"

/* Diagnostic pixel-function image. One straight-line program is evaluated for
 * each source pixel. The product cap bounds one source-image sweep. Retries,
 * reordered reads, and multiple nodes may evaluate the program again. */
#define KSN_PIXEL_CODE_MAX 24u
#define KSN_PIXEL_REGS 8u
#define KSN_PIXEL_PARAMS 8u
#define KSN_PIXEL_WORK_MAX 600000u

typedef enum {
    KSN_PIXEL_IMM = 1, KSN_PIXEL_X, KSN_PIXEL_Y, KSN_PIXEL_PARAM,
    KSN_PIXEL_UNDERLAY, KSN_PIXEL_ADD, KSN_PIXEL_SUB, KSN_PIXEL_MUL,
    KSN_PIXEL_SHR, KSN_PIXEL_AND, KSN_PIXEL_OR, KSN_PIXEL_XOR
} ksn_pixel_opcode;

/* Arithmetic wraps at 16 bits. SHR is logical, by the immediate (0..15).
 * Each instruction writes dst; binary operands must have been written earlier. */
typedef struct {
    uint8_t op,dst,a,b;
    uint16_t immediate;
} ksn_pixel_instruction;

typedef struct {
    uint16_t width,height;
    uint8_t count,color_reg,alpha_reg;
    uint16_t params[KSN_PIXEL_PARAMS];
    ksn_pixel_instruction code[KSN_PIXEL_CODE_MAX];
} ksn_pixel_frame;

/* Optional explicit underlay is a caller-owned RGB565 snapshot, never the
 * live compositor strip. It must remain immutable through all pending,
 * retry, and repair reads that use the bound image. */
typedef struct {
    const uint16_t *rgb565;
    size_t pixels;
    uint16_t stride;
} ksn_pixel_underlay;

typedef struct {
    ksn_pixel_frame frame; /* copied and frozen at bind */
    ksn_pixel_underlay underlay; /* borrowed immutable snapshot */
    uint16_t color[240];
    uint8_t alpha[240];
    uint16_t cached_y;
    bool valid,cached;
} ksn_pixel_image;

/* Zero-initialize ksn_pixel_image before its first bind. Bind validates and
 * copies the complete program and parameters. The caller
 * must not rebind an image while its Kasane resource can still be read by an
 * in-flight presentation or repair; use a separate image lease for that case.
 * Rebind keeps width and height fixed because registration snapshots them.
 * After a safe rebind the caller invalidates the registered image resource. */
bool ksn_pixel_image_bind(ksn_pixel_image *image,const ksn_pixel_frame *frame,
                          const ksn_pixel_underlay *underlay);
void ksn_pixel_image_port(ksn_pixel_image *image,ksn_image_port *out);

/* Registered port for a changing pixel function. A candidate and the last
 * presented frame occupy different slots. stage snapshots both the program
 * and an optional underlay into caller-supplied, pool-owned storage; neither
 * input may be retained by the renderer. Storage is optional for programs
 * without UNDERLAY. Initialize before registration; reinitialize only after
 * the Kasane resource has been reset. All calls share Kasane's owner task. */
#define KSN_PIXEL_SLOTS 2u
typedef struct { uint64_t generation; uint8_t slot; } ksn_pixel_handle;
typedef enum { KSN_PIXEL_OK, KSN_PIXEL_BUSY, KSN_PIXEL_INVALID }
    ksn_pixel_result;
typedef struct {
    ksn_pixel_image image;
    uint16_t *underlay_pixels;
    uint64_t generation;
} ksn_pixel_slot;
typedef struct {
    ksn_pixel_slot slots[KSN_PIXEL_SLOTS];
    uint16_t width,height;
    size_t underlay_capacity;
    ksn_pixel_handle committed,pending;
    bool repair_required;
} ksn_pixel_pool;

bool ksn_pixel_pool_init(ksn_pixel_pool *pool,uint16_t width,uint16_t height,
                         uint16_t *underlay_storage[KSN_PIXEL_SLOTS],
                         size_t pixels_per_buffer);
/* BUSY while a candidate is pending, or after discard until the old image
 * has been repaired. handle.slot is the KSN_IMAGE frame number. */
ksn_pixel_result ksn_pixel_pool_stage(ksn_pixel_pool *pool,
                                      const ksn_pixel_frame *frame,
                                      const ksn_pixel_underlay *underlay,
                                      ksn_pixel_handle *handle);
/* One current generation per registered image resource: after ACK, every
 * retained node using that resource must reference the new frame index.
 * finish(true,...,true) may retire the previous slot only when the owner has
 * checked that precondition. Passing false rejects ACK and keeps both slots.
 * On failed I/O retain pending for retry. finish(false,...,false) is only for
 * a discarded submission with no retained candidate nodes. Repair the old
 * committed image before repair_done and restaging. */
bool ksn_pixel_pool_finish(ksn_pixel_pool *pool,ksn_pixel_handle handle,
                            bool transferred,bool previous_frame_retired);
void ksn_pixel_pool_repair_done(ksn_pixel_pool *pool);
ksn_pixel_handle ksn_pixel_pool_committed(const ksn_pixel_pool *pool);
void ksn_pixel_pool_port(ksn_pixel_pool *pool,ksn_image_port *out);

#endif
