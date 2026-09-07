#include "pocket_io.h"
#include "pocket_api.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// A macro, not a pointer: watch_table below is a static initializer.
#define TAG "pocket.io"

// The numbers below are the ones this file enforces. Section 14's table is
// where most of them come from, but where the hardware disagrees the hardware
// wins and the difference is written down beside the constant.
#define IO_HANDLES          4       // open devices at once, all kinds together
#define IO_GPIO_WATCHES     4

#define I2C_MAX_BYTES       256     // each direction, section 14
#define I2C_EXT_WAIT_MS     50      // the keyboard is behind this bus
#define I2C_GROVE_WAIT_MS   100     // nothing else is on that one

#define SPI_MAX_BYTES       1024
#define SPI_MIN_HZ          100000
#define SPI_MAX_HZ          20000000
// The JS task draws the screen, so a transfer it performs inline is a frame it
// does not draw. 20ms is under a frame period at 30fps and is checked against
// the transfer's own computed length, not hoped for.
#define SPI_MAX_HOLD_US     20000

#define UART_MIN_BAUD       300
#define UART_MAX_BAUD       921600
#define UART_MAX_READ       1024
// One write at a time and at most this many bytes, so the bytes always fit the
// ring below and uart_write_bytes never blocks the JS task.
#define UART_MAX_WRITE      256
#define UART_RX_RING        512
#define UART_TX_RING        1024
#define UART_READ_TIMEOUT   1000    // section 14's default for a UART read
#define IO_MAX_TIMEOUT_MS   30000   // section 14's ceiling for any promise

#define GPIO_MAX_DEBOUNCE   1000

#define IR_MAX_DURATIONS    256
#define IR_MAX_TOTAL_US     200000
#define IR_MIN_CARRIER_HZ   20000
#define IR_MAX_CARRIER_HZ   60000
// The RMT duration field is 15 bits and the channel runs at 1MHz, so one
// mark or space cannot exceed this however generous section 14 is.
#define IR_MAX_ONE_US       32767
#define IR_GPIO             44

// ------------------------------------------------------------------- ports
//
// Section 10 refuses raw GPIO numbers and publishes validated ports instead.
// Every port here is one protocol on one pin set, so a program can see exactly
// what it is taking and this file can tell at a glance which two ports cannot
// be open at once.
//
// What is NOT here is the point of the list: the LCD (33-38), the flash, USB,
// the battery ADC, the strapping pins, the I2S lines to the codec and the SD
// card's own chip select are absent, and section 10 asks for them to be
// reserved by not being published rather than by being published and refused.

typedef enum { PROTO_I2C, PROTO_SPI, PROTO_UART, PROTO_GPIO, PROTO_IR } proto_t;

typedef struct {
    const char *id;
    proto_t     proto;
    uint8_t     pins[4];
    uint8_t     pin_count;
    const char *wiring;      // published in the port's limits
} port_t;

// GPIO8/9 carry the internal bus. They appear once, as ext.i2c, and never as a
// GPIO port: a program that could drive them as pins would stop the keyboard
// just as surely as one that wrote to 0x34.
static const port_t PORTS[] = {
    {"grove",     PROTO_I2C,  {2,1},        2, "sda=G2,scl=G1"},
    {"grove.g2",  PROTO_GPIO, {2},          1, "pin=G2"},
    {"grove.g1",  PROTO_GPIO, {1},          1, "pin=G1"},
    {"ext.i2c",   PROTO_I2C,  {8,9},        2, "sda=G8,scl=G9"},
    {"ext.spi",   PROTO_SPI,  {40,14,39,5}, 4, "sck=G40,mosi=G14,miso=G39,cs=G5"},
    {"ext.uart",  PROTO_UART, {13,15},      2, "tx=G13,rx=G15"},
    {"ext.int",   PROTO_GPIO, {4},          1, "pin=G4"},
    {"ext.busy",  PROTO_GPIO, {6},          1, "pin=G6"},
    {"ext.reset", PROTO_GPIO, {3},          1, "pin=G3"},
    {"ir",        PROTO_IR,   {IR_GPIO},    1, "tx=G44"},
};
#define PORT_COUNT (sizeof(PORTS)/sizeof(PORTS[0]))

// The addresses a guest may not open on ext.i2c. 0x18 is the ES8311 codec and
// 0x34 the TCA8418 keyboard; the IMU takes both 0x68 and 0x69 because motion.c
// probes the pair and keeps whichever answered, so neither can be handed out.
// The refusal is published as `reservedAddresses`, in this order.
static const uint8_t I2C_RESERVED[] = { 0x18, 0x34, 0x68, 0x69 };

static bool address_reserved(int port, unsigned address) {
    // 0x00-0x07 and 0x78-0x7F are reserved by I2C itself -- general call, CBUS,
    // 10-bit addressing -- and are not devices on any port.
    if(address<0x08 || address>0x77) return true;
    if(strcmp(PORTS[port].id,"ext.i2c")!=0) return false;
    for(unsigned i=0;i<sizeof(I2C_RESERVED);i++)
        if(I2C_RESERVED[i]==address) return true;
    return false;
}

// Which GPIOs are spoken for. Two ports that share a pin -- grove and its two
// GPIO ports are the only such pair today -- cannot both be open, and the
// second one asks gets BUSY rather than a silently reconfigured pad.
static uint64_t claimed_pins;

static bool pins_free(int port) {
    for(unsigned i=0;i<PORTS[port].pin_count;i++)
        if(claimed_pins & (1ULL<<PORTS[port].pins[i])) return false;
    return true;
}
static void claim_pins(int port, bool taken) {
    for(unsigned i=0;i<PORTS[port].pin_count;i++) {
        uint64_t bit=1ULL<<PORTS[port].pins[i];
        if(taken) claimed_pins|=bit; else claimed_pins&=~bit;
    }
}

static int find_port(const char *id, proto_t proto) {
    for(unsigned i=0;i<PORT_COUNT;i++)
        if(PORTS[i].proto==proto && !strcmp(PORTS[i].id,id)) return (int)i;
    return -1;
}

// ----------------------------------------------------------------- handles
//
// A handle is a slot number and the generation that slot was on when the
// program was given it, packed into one integer that travels as the bound data
// of the handle's own methods. Looking one up is a bounds check and a
// comparison, and a handle from a closed device -- or from a session that has
// ended -- finds a generation that has moved on and is answered with CLOSED.
// There is no JS class and no finalizer, so nothing native outlives close().

typedef enum { H_FREE=0, H_I2C, H_SPI, H_UART, H_GPIO } handle_kind_t;

typedef struct {
    handle_kind_t kind;
    uint16_t      generation;
    int           port;

    i2c_master_dev_handle_t i2c_dev;
    int                     i2c_wait_ms;

    spi_device_handle_t spi_dev;
    uint8_t            *spi_tx, *spi_rx;   // DMA-capable, only while open
    uint32_t            spi_hz;

    pocket_request_t uart_read;   // 0 when no read is in flight
    uint32_t         uart_want;

    bool gpio_output;
    int  gpio_watch;              // -1 when the pin is not watched
} io_handle_t;

static io_handle_t handles[IO_HANDLES];
// Which slot the open() that is running has filled. The four builders and
// finish_open() are one call apart on the same task, so this is a return
// value with a shorter signature and not a piece of state.
static int opened_slot=-1;

#define HANDLE_SLOT_BITS 4
static int32_t handle_id(int slot) {
    return (int32_t)(((uint32_t)handles[slot].generation<<HANDLE_SLOT_BITS)|
                     (uint32_t)slot);
}
static io_handle_t *slot_of(int32_t id) {
    int slot=id & ((1<<HANDLE_SLOT_BITS)-1);
    if(id<0 || slot>=IO_HANDLES) return NULL;
    io_handle_t *h=&handles[slot];
    if(h->kind==H_FREE || h->generation!=(uint16_t)(id>>HANDLE_SLOT_BITS))
        return NULL;
    return h;
}
static int free_slot(void) {
    for(int i=0;i<IO_HANDLES;i++) if(handles[i].kind==H_FREE) return i;
    return -1;
}

// ------------------------------------------------------------- i2c buses
//
// ext.i2c is the bus board_init() already made; this file borrows it by port
// number and never deletes it. Grove is a second bus on GPIO2/1, created when
// the first grove device opens and deleted when the last one closes, so a
// program that does not use it pays nothing.

static i2c_master_bus_handle_t grove_bus;
static unsigned                grove_users;

static esp_err_t bus_for(int port, i2c_master_bus_handle_t *out) {
    if(!strcmp(PORTS[port].id,"ext.i2c"))
        return i2c_master_get_bus_handle(I2C_NUM_0,out);
    if(!grove_bus) {
        i2c_master_bus_config_t cfg = {
            .i2c_port=I2C_NUM_1, .sda_io_num=2, .scl_io_num=1,
            .clk_source=I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt=7,
            .flags.enable_internal_pullup=true,
        };
        esp_err_t e=i2c_new_master_bus(&cfg,&grove_bus);
        if(e!=ESP_OK) { grove_bus=NULL; return e; }
    }
    *out=grove_bus;
    return ESP_OK;
}

// ---------------------------------------------------------------- teardown

static void gpio_release(int pin) {
    // Section 10 wants a defined state at close. A floating input is the one
    // state that cannot drive anything the program left attached, and it is
    // what these pins are before anybody opens them.
    gpio_config_t cfg = {
        .pin_bit_mask=1ULL<<pin, .mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_DISABLE, .pull_down_en=GPIO_PULLDOWN_DISABLE,
        .intr_type=GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static void watch_release(int slot);   // defined with the subscription table

static void handle_teardown(io_handle_t *h) {
    switch(h->kind) {
        case H_I2C:
            if(h->i2c_dev) i2c_master_bus_rm_device(h->i2c_dev);
            if(strcmp(PORTS[h->port].id,"ext.i2c")!=0 && grove_users &&
               --grove_users==0 && grove_bus) {
                i2c_del_master_bus(grove_bus);
                grove_bus=NULL;
            }
            break;
        case H_SPI:
            if(h->spi_dev) spi_bus_remove_device(h->spi_dev);
            spi_bus_free(SPI3_HOST);
            free(h->spi_tx); free(h->spi_rx);
            break;
        case H_UART:
            uart_driver_delete(UART_NUM_1);
            // The pins go back to inputs; a TX line left driving would keep
            // feeding whatever is attached after the program has gone.
            gpio_release(13); gpio_release(15);
            break;
        case H_GPIO:
            if(h->gpio_watch>=0) watch_release(h->gpio_watch);
            gpio_release(PORTS[h->port].pins[0]);
            break;
        default: return;
    }
    claim_pins(h->port,false);
    h->kind=H_FREE;
    h->i2c_dev=NULL; h->spi_dev=NULL; h->spi_tx=NULL; h->spi_rx=NULL;
    h->uart_read=0; h->gpio_watch=-1;
}

// Ends a handle from JS. The generation moves first, so every method already
// handed out becomes CLOSED before any hardware goes away, and a read still in
// flight is told to settle as CLOSED rather than being left to a deadline.
static void handle_close(io_handle_t *h) {
    pocket_request_t pending=h->uart_read;
    h->generation++;
    h->uart_read=0;
    handle_teardown(h);
    if(pending) pocket_api_complete(pending,1);
}

// ------------------------------------------------------------------ options

typedef struct {
    int32_t timeout_ms;   // 0 for "not given"
    JSValue cancel;       // JS_UNDEFINED when none
    bool    cancelled;
} io_options_t;

// The same shape pocket_av.c uses, with the ceiling passed in: an I2C wait and
// a UART read do not share a maximum, and section 4 refuses to round an
// over-range request quietly rather than clamping it.
static JSValue take_options(JSContext *ctx, JSValueConst value,
                            const char *operation, int32_t max_ms,
                            io_options_t *out) {
    out->timeout_ms=0; out->cancel=JS_UNDEFINED; out->cancelled=false;
    if(JS_IsUndefined(value) || JS_IsNull(value)) return JS_UNDEFINED;
    if(!JS_IsObject(value))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                 "options must be an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    JSValue timeout=JS_GetPropertyStr(ctx,value,"timeoutMs");
    if(JS_IsException(timeout)) return JS_EXCEPTION;
    if(!JS_IsUndefined(timeout) && !JS_IsNull(timeout)) {
        double ms=0;
        bool bad=!JS_IsNumber(timeout) || JS_ToFloat64(ctx,&ms,timeout);
        JS_FreeValue(ctx,timeout);
        if(bad || !isfinite(ms) || ms!=(double)(int64_t)ms || ms<1 || ms>max_ms)
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                     "timeoutMs is outside this operation's range",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        out->timeout_ms=(int32_t)ms;
    } else JS_FreeValue(ctx,timeout);

    JSValue cancel=JS_GetPropertyStr(ctx,value,"cancel");
    if(JS_IsException(cancel)) return JS_EXCEPTION;
    if(!JS_IsUndefined(cancel) && !JS_IsNull(cancel)) {
        if(!pocket_api_is_cancel_token(cancel)) {
            JS_FreeValue(ctx,cancel);
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                     "cancel must be a token from pocket.cancel.source()",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        }
        out->cancelled=pocket_api_cancel_requested(cancel);
        out->cancel=cancel;
        return JS_UNDEFINED;
    }
    JS_FreeValue(ctx,cancel);
    return JS_UNDEFINED;
}

// Reads a whole-number property. Returns false and leaves an argument
// rejection in *error when the value is missing or not one.
static bool take_int(JSContext *ctx, JSValueConst object, const char *name,
                     const char *operation, double lo, double hi,
                     int64_t *out, JSValue *error) {
    JSValue field=JS_GetPropertyStr(ctx,object,name);
    if(JS_IsException(field)) { *error=JS_EXCEPTION; return false; }
    double v=0;
    bool bad=!JS_IsNumber(field) || JS_ToFloat64(ctx,&v,field);
    JS_FreeValue(ctx,field);
    if(bad || !isfinite(v) || v!=(double)(int64_t)v || v<lo || v>hi) {
        *error=pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,name,
                                 false,POCKET_OUTCOME_NOT_APPLIED);
        return false;
    }
    *out=(int64_t)v;
    return true;
}

// The port argument, resolved and checked for a conflicting owner in one place
// so every open() refuses the same way.
static int take_port(JSContext *ctx, JSValueConst spec, proto_t proto,
                     const char *operation, JSValue *error) {
    JSValue value=JS_GetPropertyStr(ctx,spec,"port");
    if(JS_IsException(value)) { *error=JS_EXCEPTION; return -1; }
    const char *id=JS_ToCString(ctx,value);
    JS_FreeValue(ctx,value);
    if(!id) {
        *error=pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,operation,
                                 "port must be a string from pocket.io.ports()",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
        return -1;
    }
    int port=find_port(id,proto);
    JS_FreeCString(ctx,id);
    if(port<0) {
        // NOT_FOUND, not UNSUPPORTED: the protocol is implemented, this name
        // just is not one of its ports on this board.
        *error=pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,operation,
                                 "no such port for this protocol",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
        return -1;
    }
    if(!pins_free(port)) {
        *error=pocket_api_reject(ctx,POCKET_ERR_BUSY,operation,
                                 "another handle holds a pin of this port",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
        return -1;
    }
    return port;
}

// ------------------------------------------------------------- gpio watches

typedef struct {
    int32_t  id;            // the handle the watch belongs to
    int      pin;
    int      edge;          // -1 falling, 1 rising, 0 both
    int64_t  debounce_us;
    int      level;         // the level currently believed settled
    int      candidate;     // what the pin has been reading
    int64_t  since_us;      // when the candidate first appeared
    bool     pending;       // an edge is waiting to be delivered this pump
    int64_t  time_us;
    uint32_t dropped;
} watch_t;

static pocket_sub_slot_t watch_slots[IO_GPIO_WATCHES];
static watch_t           watch_state[IO_GPIO_WATCHES];
static pocket_sub_table_t watch_table = {
    .slots=watch_slots, .count=IO_GPIO_WATCHES,
    .tag=TAG, .what="gpio watch",
    // The same rule the polled surfaces use: a listener that throws on every
    // edge would otherwise fill the log for as long as the pin keeps moving.
    .close_on_throw=true,
};

static void watch_release(int slot) {
    if(slot<0 || slot>=IO_GPIO_WATCHES) return;
    watch_state[slot].id=0;
    pocket_api_sub_close(&watch_table,slot);
}

// ------------------------------------------------------------------- i2c

static JSValue i2c_transfer(JSContext *ctx, io_handle_t *h,
                            int argc, JSValueConst *argv) {
    static const char *const OP="io.i2c.transfer";
    if(argc<1 || !JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "transfer(spec, options) needs a spec object",
                                 false,POCKET_OUTCOME_NOT_APPLIED);

    uint8_t out[I2C_MAX_BYTES], in[I2C_MAX_BYTES];
    size_t  write_len=0;
    JSValue write=JS_GetPropertyStr(ctx,argv[0],"write");
    if(JS_IsException(write)) return JS_EXCEPTION;
    if(!JS_IsUndefined(write) && !JS_IsNull(write)) {
        size_t   size=0;
        uint8_t *bytes=JS_GetUint8Array(ctx,&size,write);
        if(!bytes) {
            JS_FreeValue(ctx,write);
            JS_FreeValue(ctx,JS_GetException(ctx));
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                     "write must be a Uint8Array",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        }
        if(size>I2C_MAX_BYTES) {
            JS_FreeValue(ctx,write);
            return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                     "write is longer than maxTransferBytes",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        }
        // Section 4 copies at acceptance, so a program that mutates the array
        // afterwards cannot change what went out.
        memcpy(out,bytes,size);
        write_len=size;
    }
    JS_FreeValue(ctx,write);

    int64_t read_len=0;
    JSValue error=JS_UNDEFINED;
    JSValue field=JS_GetPropertyStr(ctx,argv[0],"readBytes");
    bool given=!JS_IsUndefined(field) && !JS_IsNull(field);
    JS_FreeValue(ctx,field);
    if(given && !take_int(ctx,argv[0],"readBytes",OP,0,I2C_MAX_BYTES,
                          &read_len,&error)) return error;
    if(!write_len && !read_len)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "a transfer needs write bytes, readBytes or both",
                                 false,POCKET_OUTCOME_NOT_APPLIED);

    io_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,
                             h->i2c_wait_ms,&options);
    if(!JS_IsUndefined(bad)) return bad;
    JS_FreeValue(ctx,options.cancel);
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the transfer",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    int wait=options.timeout_ms?options.timeout_ms:h->i2c_wait_ms;

    // The bus is entered here and left before this function returns: one
    // transaction, on the JS task, inside the driver's own per-bus mutex, with
    // `wait` covering the queue as well as the wire. That is the entire window
    // in which the keyboard can be waiting on a guest.
    esp_err_t e;
    if(write_len && read_len)
        e=i2c_master_transmit_receive(h->i2c_dev,out,write_len,in,
                                      (size_t)read_len,wait);
    else if(write_len)
        e=i2c_master_transmit(h->i2c_dev,out,write_len,wait);
    else
        e=i2c_master_receive(h->i2c_dev,in,(size_t)read_len,wait);

    if(e!=ESP_OK) {
        // A NACK and a timeout are different things to a program: nothing
        // answered, versus something answered too slowly. Both leave the write
        // in an unknown state, because the device may have taken the bytes and
        // then failed to acknowledge the last of them.
        const char *code=e==ESP_ERR_TIMEOUT?POCKET_ERR_TIMEOUT
                                           :POCKET_ERR_IO_ERROR;
        return pocket_api_reject(ctx,code,OP,esp_err_to_name(e),true,
                                 write_len?POCKET_OUTCOME_UNKNOWN
                                          :POCKET_OUTCOME_NOT_APPLIED);
    }
    // JS-owned bytes, copied out of the stack buffer: section 4 forbids handing
    // a driver's buffer to the guest.
    return pocket_api_settled(ctx,JS_NewUint8ArrayCopy(ctx,in,(size_t)read_len),
                              false);
}

static JSValue i2c_open(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="io.i2c.open";
    if(argc<1 || !JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "open(spec, options) needs a spec object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValue error=JS_UNDEFINED;
    int port=take_port(ctx,argv[0],PROTO_I2C,OP,&error);
    if(port<0) return error;

    int64_t address=0, hz=0;
    if(!take_int(ctx,argv[0],"address",OP,0,0x7f,&address,&error)) return error;
    if(!take_int(ctx,argv[0],"hz",OP,100000,400000,&hz,&error)) return error;
    // Section 10 types hz as 100000|400000, not as a range, and the two are the
    // only speeds the devices on the shared bus are set up for.
    if(hz!=100000 && hz!=400000)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "hz must be 100000 or 400000",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(address_reserved(port,(unsigned)address))
        // Guard 1 of the header. PERMISSION_DENIED rather than
        // INVALID_ARGUMENT: the address is well formed, the program is simply
        // not allowed to have it.
        return pocket_api_reject(ctx,POCKET_ERR_PERMISSION_DENIED,OP,
                                 "this address belongs to the host",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    int slot=free_slot();
    if(slot<0)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "too many open handles",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    i2c_master_bus_handle_t bus=NULL;
    esp_err_t e=bus_for(port,&bus);
    if(e!=ESP_OK || !bus)
        return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,OP,
                                 "the bus could not be opened",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    i2c_device_config_t cfg = {
        .dev_addr_length=I2C_ADDR_BIT_LEN_7,
        .device_address=(uint16_t)address, .scl_speed_hz=(uint32_t)hz,
    };
    io_handle_t *h=&handles[slot];
    e=i2c_master_bus_add_device(bus,&cfg,&h->i2c_dev);
    if(e!=ESP_OK) {
        h->i2c_dev=NULL;
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,esp_err_to_name(e),true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    h->kind=H_I2C; h->port=port; h->gpio_watch=-1;
    h->i2c_wait_ms=!strcmp(PORTS[port].id,"ext.i2c")?I2C_EXT_WAIT_MS
                                                    :I2C_GROVE_WAIT_MS;
    if(strcmp(PORTS[port].id,"ext.i2c")!=0) grove_users++;
    claim_pins(port,true);
    opened_slot=slot;
    return JS_UNDEFINED;    // finish_open() builds the device object
}

// -------------------------------------------------------------------- spi

static JSValue spi_transfer(JSContext *ctx, io_handle_t *h,
                            int argc, JSValueConst *argv) {
    static const char *const OP="io.spi.transfer";
    size_t   length=0;
    uint8_t *bytes=argc>0?JS_GetUint8Array(ctx,&length,argv[0]):NULL;
    if(!bytes) {
        JS_FreeValue(ctx,JS_GetException(ctx));
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "transfer(tx) needs a Uint8Array",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(!length || length>SPI_MAX_BYTES)
        return pocket_api_reject(ctx,
            length?POCKET_ERR_LIMIT_EXCEEDED:POCKET_ERR_INVALID_ARGUMENT,OP,
            "tx must be 1 to maxTransferBytes bytes",false,
            POCKET_OUTCOME_NOT_APPLIED);

    io_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,
                             IO_MAX_TIMEOUT_MS,&options);
    if(!JS_IsUndefined(bad)) return bad;
    JS_FreeValue(ctx,options.cancel);
    if(options.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the transfer",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    // spi_device_polling_transmit takes no deadline: it returns when the bits
    // have gone. So the deadline is enforced before the transfer instead, out
    // of the one thing that is knowable in advance -- how long the bits take at
    // the clock this handle was opened with. SPI_MAX_HOLD_US is the ceiling
    // whatever timeoutMs says, because the JS task is also the drawing task.
    int64_t bits=(int64_t)length*8;
    int64_t needed_us=bits*1000000/h->spi_hz+50;
    int64_t allowed_us=options.timeout_ms?
        (int64_t)options.timeout_ms*1000:SPI_MAX_HOLD_US;
    if(allowed_us>SPI_MAX_HOLD_US) allowed_us=SPI_MAX_HOLD_US;
    if(needed_us>allowed_us)
        return pocket_api_reject(ctx,POCKET_ERR_TIMEOUT,OP,
                                 "this many bytes at this clock exceeds maxHoldMs",
                                 false,POCKET_OUTCOME_NOT_APPLIED);

    memcpy(h->spi_tx,bytes,length);
    memset(h->spi_rx,0,length);
    // Section 10 asks for the chip select to be released before control goes
    // back to JS. One transaction with no SPI_TRANS_CS_KEEP_ACTIVE is exactly
    // that: the driver raises CS when the transaction ends.
    spi_transaction_t t = {
        .length=(size_t)bits, .tx_buffer=h->spi_tx, .rx_buffer=h->spi_rx,
    };
    esp_err_t e=spi_device_polling_transmit(h->spi_dev,&t);
    if(e!=ESP_OK)
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,esp_err_to_name(e),
                                 true,POCKET_OUTCOME_UNKNOWN);
    return pocket_api_settled(ctx,JS_NewUint8ArrayCopy(ctx,h->spi_rx,length),
                              false);
}

static JSValue spi_open(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="io.spi.open";
    if(argc<1 || !JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "open(spec, options) needs a spec object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValue error=JS_UNDEFINED;
    int port=take_port(ctx,argv[0],PROTO_SPI,OP,&error);
    if(port<0) return error;
    int64_t mode=0, hz=0;
    if(!take_int(ctx,argv[0],"mode",OP,0,3,&mode,&error)) return error;
    if(!take_int(ctx,argv[0],"hz",OP,SPI_MIN_HZ,SPI_MAX_HZ,&hz,&error))
        return error;

    int slot=free_slot();
    if(slot<0)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "too many open handles",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    io_handle_t *h=&handles[slot];
    // DMA, because without it one transaction cannot exceed the 64-byte
    // hardware buffer, and DMA-capable memory, because the descriptors read it
    // directly. Two kilobytes, held only while a SPI device is open.
    h->spi_tx=heap_caps_malloc(SPI_MAX_BYTES,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    h->spi_rx=heap_caps_malloc(SPI_MAX_BYTES,MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL);
    if(!h->spi_tx || !h->spi_rx) {
        free(h->spi_tx); free(h->spi_rx); h->spi_tx=NULL; h->spi_rx=NULL;
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no room for the transfer buffers",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    // SPI3, not the SPI2 board.c drives the panel with. These are the microSD
    // pins; no SD driver exists in this build, so the host is free, and the bus
    // is created here and freed on close rather than held for the whole run.
    spi_bus_config_t bus = {
        .mosi_io_num=14, .miso_io_num=39, .sclk_io_num=40,
        .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=SPI_MAX_BYTES,
    };
    esp_err_t e=spi_bus_initialize(SPI3_HOST,&bus,SPI_DMA_CH_AUTO);
    if(e!=ESP_OK) {
        free(h->spi_tx); free(h->spi_rx); h->spi_tx=NULL; h->spi_rx=NULL;
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,esp_err_to_name(e),true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz=(int)hz, .mode=(uint8_t)mode, .spics_io_num=5,
        .queue_size=1,
    };
    e=spi_bus_add_device(SPI3_HOST,&dev,&h->spi_dev);
    if(e!=ESP_OK) {
        spi_bus_free(SPI3_HOST);
        free(h->spi_tx); free(h->spi_rx); h->spi_tx=NULL; h->spi_rx=NULL;
        h->spi_dev=NULL;
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,esp_err_to_name(e),true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    h->kind=H_SPI; h->port=port; h->gpio_watch=-1; h->spi_hz=(uint32_t)hz;
    claim_pins(port,true);
    opened_slot=slot;
    return JS_UNDEFINED;
}

// ------------------------------------------------------------------- uart

// The only statuses a UART read posts besides POCKET_STATUS_OK.
#define UART_STATUS_CLOSED 1

static JSValue uart_read_settle(JSContext *ctx, void *user, int32_t status,
                                const char *stop_code, bool *rejected) {
    io_handle_t *h=slot_of((int32_t)(intptr_t)user);
    *rejected=true;
    if(!h || status==UART_STATUS_CLOSED)
        return pocket_api_error(ctx,POCKET_ERR_CLOSED,"io.uart.read",
                                "the port closed while the read was waiting",
                                false,POCKET_OUTCOME_NOT_APPLIED);
    if(stop_code)
        // Section 10: a read that produced nothing is a TIMEOUT, never an
        // empty array, so that an empty result cannot be read as end of input.
        return pocket_api_error(ctx,stop_code,"io.uart.read",
                                !strcmp(stop_code,POCKET_ERR_TIMEOUT)
                                    ?"no bytes arrived before timeoutMs"
                                    :"cancelled while waiting for bytes",
                                true,POCKET_OUTCOME_NOT_APPLIED);
    uint8_t bytes[UART_MAX_READ];
    uint32_t want=h->uart_want<UART_MAX_READ?h->uart_want:UART_MAX_READ;
    int got=uart_read_bytes(UART_NUM_1,bytes,want,0);
    if(got<=0)
        return pocket_api_error(ctx,POCKET_ERR_IO_ERROR,"io.uart.read",
                                "the bytes went away between the poll and the read",
                                true,POCKET_OUTCOME_NOT_APPLIED);
    *rejected=false;
    return JS_NewUint8ArrayCopy(ctx,bytes,(size_t)got);
}

static void uart_read_stop(void *user, const char *code) {
    (void)code;
    // Nothing native is outstanding -- the driver's ISR fills its ring whether
    // anybody is waiting or not -- so the stop is just a completion the pump
    // will turn into the TIMEOUT or CANCELLED that pocket_api.c asked for.
    io_handle_t *h=slot_of((int32_t)(intptr_t)user);
    if(h && h->uart_read) pocket_api_complete(h->uart_read,POCKET_STATUS_OK);
}

static void uart_read_release(void *user) {
    io_handle_t *h=slot_of((int32_t)(intptr_t)user);
    if(h) h->uart_read=0;
}

static const pocket_promise_ops_t uart_read_ops = {
    .settle=uart_read_settle, .stop=uart_read_stop, .release=uart_read_release,
};

static JSValue uart_read(JSContext *ctx, io_handle_t *h, int32_t id,
                         int argc, JSValueConst *argv) {
    static const char *const OP="io.uart.read";
    int64_t want=0;
    if(argc<1 || !JS_IsNumber(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "read(maxBytes, options) needs a number",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    {
        double v=0;
        if(JS_ToFloat64(ctx,&v,argv[0]) || !isfinite(v) ||
           v!=(double)(int64_t)v || v<1 || v>UART_MAX_READ)
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                     "maxBytes must be 1 to maxReadBytes",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        want=(int64_t)v;
    }
    // Section 4 allows one operation of a kind per handle and answers the
    // second with BUSY.
    if(h->uart_read)
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "a read is already waiting on this port",true,
                                 POCKET_OUTCOME_NOT_APPLIED);

    io_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,
                             IO_MAX_TIMEOUT_MS,&options);
    if(!JS_IsUndefined(bad)) return bad;
    if(options.cancelled) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the read",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    h->uart_want=(uint32_t)want;
    h->uart_read=request;
    int64_t deadline=esp_timer_get_time()+1000LL*
        (options.timeout_ms?options.timeout_ms:UART_READ_TIMEOUT);
    JSValue promise=pocket_api_promise_arm(ctx,request,&uart_read_ops,
                                           (void *)(intptr_t)id,options.cancel,
                                           deadline);
    if(JS_IsException(promise)) h->uart_read=0;
    return promise;
}

static JSValue uart_write(JSContext *ctx, io_handle_t *h,
                          int argc, JSValueConst *argv) {
    static const char *const OP="io.uart.write";
    size_t   length=0;
    uint8_t *bytes=argc>0?JS_GetUint8Array(ctx,&length,argv[0]):NULL;
    if(!bytes) {
        JS_FreeValue(ctx,JS_GetException(ctx));
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "write(data) needs a Uint8Array",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(!length || length>UART_MAX_WRITE)
        return pocket_api_reject(ctx,
            length?POCKET_ERR_LIMIT_EXCEEDED:POCKET_ERR_INVALID_ARGUMENT,OP,
            "data must be 1 to maxWriteBytes bytes",false,
            POCKET_OUTCOME_NOT_APPLIED);
    // The one thing that makes this call safe to run inline: at most
    // UART_MAX_WRITE bytes may be in the driver's UART_TX_RING at a time, so
    // uart_write_bytes always has room and never blocks the drawing task
    // waiting for a slow baud rate to drain. The refusal is what enforces it.
    if(uart_wait_tx_done(UART_NUM_1,0)!=ESP_OK)
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "the previous write has not left the port yet",
                                 true,POCKET_OUTCOME_NOT_APPLIED);
    int sent=uart_write_bytes(UART_NUM_1,bytes,length);
    if(sent<0)
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,"the write failed",
                                 true,POCKET_OUTCOME_UNKNOWN);
    // Queued, not yet on the wire. Section 4 wants that admitted rather than
    // dressed up as delivery, which is why the resolved value is a byte count
    // and not a completion.
    return pocket_api_settled(ctx,JS_NewInt32(ctx,sent),false);
}

static JSValue uart_open(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="io.uart.open";
    if(argc<1 || !JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "open(spec, options) needs a spec object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValue error=JS_UNDEFINED;
    int port=take_port(ctx,argv[0],PROTO_UART,OP,&error);
    if(port<0) return error;
    int64_t baud=0;
    if(!take_int(ctx,argv[0],"baud",OP,UART_MIN_BAUD,UART_MAX_BAUD,&baud,&error))
        return error;

    int slot=free_slot();
    if(slot<0)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "too many open handles",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    // UART1. UART0's pins are the IR LED's neighbours and the console is USB
    // Serial/JTAG, but UART0 is still the ROM's port and is left alone.
    uart_config_t cfg = {
        .baud_rate=(int)baud, .data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE, .stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE, .source_clk=UART_SCLK_DEFAULT,
    };
    esp_err_t e=uart_driver_install(UART_NUM_1,UART_RX_RING,UART_TX_RING,0,
                                    NULL,0);
    if(e==ESP_OK) e=uart_param_config(UART_NUM_1,&cfg);
    if(e==ESP_OK) e=uart_set_pin(UART_NUM_1,13,15,UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE);
    if(e!=ESP_OK) {
        uart_driver_delete(UART_NUM_1);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,esp_err_to_name(e),true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    io_handle_t *h=&handles[slot];
    h->kind=H_UART; h->port=port; h->gpio_watch=-1; h->uart_read=0;
    claim_pins(port,true);
    opened_slot=slot;
    return JS_UNDEFINED;
}

// ------------------------------------------------------------------- gpio

static JSValue gpio_open(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="io.gpio.open";
    if(argc<1 || !JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "open(spec, options) needs a spec object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValue error=JS_UNDEFINED;
    int port=take_port(ctx,argv[0],PROTO_GPIO,OP,&error);
    if(port<0) return error;

    JSValue value=JS_GetPropertyStr(ctx,argv[0],"mode");
    if(JS_IsException(value)) return JS_EXCEPTION;
    const char *mode=JS_ToCString(ctx,value);
    JS_FreeValue(ctx,value);
    bool output=mode && !strcmp(mode,"output");
    bool known=mode && (output || !strcmp(mode,"input"));
    if(mode) JS_FreeCString(ctx,mode);
    if(!known)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "mode must be input or output",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    int64_t initial=0;
    value=JS_GetPropertyStr(ctx,argv[0],"initial");
    bool given=!JS_IsUndefined(value) && !JS_IsNull(value);
    JS_FreeValue(ctx,value);
    if(given && !take_int(ctx,argv[0],"initial",OP,0,1,&initial,&error))
        return error;
    if(given && !output)
        // Section 10 asks for an unhandled argument to be refused rather than
        // ignored, and an input has no level to start at.
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "initial applies to an output only",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    int slot=free_slot();
    if(slot<0)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "too many open handles",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    int pin=PORTS[port].pins[0];
    // The level is written before the pad becomes an output, so the pin never
    // shows the previous contents of the output register on its way to the
    // value the program asked for.
    if(output) gpio_set_level(pin,(uint32_t)initial);
    gpio_config_t cfg = {
        .pin_bit_mask=1ULL<<pin,
        .mode=output?GPIO_MODE_OUTPUT:GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_DISABLE, .pull_down_en=GPIO_PULLDOWN_DISABLE,
        .intr_type=GPIO_INTR_DISABLE,
    };
    if(gpio_config(&cfg)!=ESP_OK)
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,
                                 "the pin could not be configured",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    io_handle_t *h=&handles[slot];
    h->kind=H_GPIO; h->port=port; h->gpio_output=output; h->gpio_watch=-1;
    claim_pins(port,true);
    opened_slot=slot;
    return JS_UNDEFINED;
}

static JSValue gpio_watch(JSContext *ctx, io_handle_t *h, int32_t id,
                          int argc, JSValueConst *argv) {
    static const char *const OP="io.gpio.watch";
    if(argc<2 || !JS_IsObject(argv[0]) || !JS_IsFunction(ctx,argv[1]))
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "watch(spec, listener) needs a spec and a function",
                                false,NULL);
    if(h->gpio_output)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "an output pin has no edges to watch",false,NULL);
    // close_on_throw may have taken the slot away underneath the handle, so
    // "already watched" is asked of the table rather than of this field.
    if(h->gpio_watch>=0 && !watch_slots[h->gpio_watch].handle) h->gpio_watch=-1;
    if(h->gpio_watch>=0)
        return pocket_api_throw(ctx,POCKET_ERR_BUSY,OP,
                                "this pin is already watched",true,NULL);

    JSValue value=JS_GetPropertyStr(ctx,argv[0],"edge");
    if(JS_IsException(value)) return JS_EXCEPTION;
    const char *edge=JS_ToCString(ctx,value);
    JS_FreeValue(ctx,value);
    int which=2;
    if(edge) {
        if(!strcmp(edge,"rising")) which=1;
        else if(!strcmp(edge,"falling")) which=-1;
        else if(!strcmp(edge,"both")) which=0;
        JS_FreeCString(ctx,edge);
    }
    if(which==2)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "edge must be rising, falling or both",false,NULL);
    int64_t debounce=0;
    JSValue error=JS_UNDEFINED;
    if(!take_int(ctx,argv[0],"debounceMs",OP,0,GPIO_MAX_DEBOUNCE,&debounce,
                 &error)) {
        // take_int builds a rejection; watch is synchronous, so the same error
        // has to be thrown instead.
        JS_FreeValue(ctx,error);
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "debounceMs must be 0 to 1000",false,NULL);
    }

    int slot=0;
    JSValue subscription=pocket_api_sub_open(ctx,&watch_table,argv[1],OP,
                                             "too many watches",&slot);
    if(JS_IsException(subscription)) return subscription;
    watch_t *w=&watch_state[slot];
    int pin=PORTS[h->port].pins[0];
    w->id=id; w->pin=pin; w->edge=which;
    w->debounce_us=debounce*1000;
    w->level=gpio_get_level(pin);
    w->candidate=w->level;
    w->since_us=esp_timer_get_time();
    w->pending=false; w->dropped=0;
    h->gpio_watch=slot;
    return subscription;
}

static bool watch_payload(JSContext *ctx, int slot, void *user, JSValue *payload) {
    (void)user;
    watch_t *w=&watch_state[slot];
    if(!w->pending) return false;
    w->pending=false;
    JSValue event=JS_NewObject(ctx);
    if(JS_IsException(event)) return false;
    JS_SetPropertyStr(ctx,event,"value",JS_NewInt32(ctx,w->level));
    JS_SetPropertyStr(ctx,event,"timeMs",JS_NewFloat64(ctx,w->time_us/1000.0));
    JS_SetPropertyStr(ctx,event,"dropped",JS_NewUint32(ctx,w->dropped));
    *payload=event;
    return true;
}

// Edges are found by sampling, once a frame, from the JS task. Section 10 asks
// only that a GPIO callback not be reached from an ISR and that it be debounced
// and rate limited; polling is all three at once, and it costs no interrupt on
// a device whose interrupts already belong to the keyboard, the IMU and the
// panel. What it cannot do is see a pulse shorter than a frame, so the io.gpio
// capability publishes edgeDetection="polled" and pollMs rather than implying
// that every transition is caught.
static void watch_pump(void) {
    int64_t now=esp_timer_get_time();
    for(int i=0;i<IO_GPIO_WATCHES;i++) {
        if(!watch_slots[i].handle) continue;
        watch_t *w=&watch_state[i];
        int level=gpio_get_level(w->pin);
        if(level!=w->candidate) { w->candidate=level; w->since_us=now; continue; }
        if(level==w->level || now-w->since_us<w->debounce_us) continue;
        w->level=level;
        w->time_us=now;
        bool wanted=w->edge==0 || (w->edge>0)==(level!=0);
        if(!wanted) continue;
        // An edge that arrives while the previous one is still undelivered is
        // counted, not queued: section 4 asks for a bounded surface with a
        // dropped count rather than a growing backlog.
        if(w->pending) w->dropped++;
        w->pending=true;
    }
    pocket_api_sub_deliver(&watch_table,watch_payload,NULL);
}

// --------------------------------------------------------------------- ir
//
// One transmission at a time on GPIO44. The channel and its encoder are built
// on the first send and released by pocket_io_reset(), so a program that never
// sends pays neither the RMT block nor the interrupt.

#define IR_STATUS_ABORTED 1

static rmt_channel_handle_t ir_channel;
static rmt_encoder_handle_t ir_encoder;
// Taken on the first frame sent and given back in pocket_io_reset(), for the
// same reason pocket_fs.c gives for its index: 512 bytes of .bss standing
// empty is 512 bytes an app that never touches the emitter cannot use, and on
// this board the scarce thing is the room a running app has.
static rmt_symbol_word_t   *ir_symbols;
static bool                 ir_busy;
// The request the ISR posts to. It travels in a static rather than in the
// channel's user_data because CONFIG_RMT_TX_ISR_CACHE_SAFE, off in this build
// but a Kconfig switch away, requires user_data to be a real pointer into
// internal RAM -- and a request number packed into a void* is not one. One
// frame is in the air at a time, so one variable is exact.
static pocket_request_t     ir_request;

static bool ir_done(rmt_channel_handle_t channel,
                    const rmt_tx_done_event_data_t *event, void *user) {
    (void)channel; (void)event; (void)user;
    // ISR context. pocket_api_complete is documented as safe from one -- no
    // lock, no allocation, no JS value -- and the RMT interrupt is not
    // cache-safe in this build, so it cannot fire while the flash cache is off
    // and reach a function that lives in flash.
    pocket_api_complete(ir_request,POCKET_STATUS_OK);
    return false;
}

static void ir_stop(void *user, const char *code) {
    (void)code;
    // Disabling the channel aborts the transmission, and the done callback may
    // never run for it, so the completion is posted here instead. The status
    // says the carrier stopped mid-frame, which is what makes the outcome
    // below honest.
    if(ir_channel) rmt_disable(ir_channel);
    pocket_api_complete((pocket_request_t)(uintptr_t)user,IR_STATUS_ABORTED);
}

static JSValue ir_settle(JSContext *ctx, void *user, int32_t status,
                         const char *stop_code, bool *rejected) {
    (void)user;
    if(ir_channel) rmt_disable(ir_channel);
    *rejected=true;
    if(stop_code)
        return pocket_api_error(ctx,stop_code,"io.ir.send",
                                !strcmp(stop_code,POCKET_ERR_TIMEOUT)
                                    ?"the frame outlived timeoutMs"
                                    :"cancelled while transmitting",
                                false,
                                status==POCKET_STATUS_OK?POCKET_OUTCOME_APPLIED
                                                        :POCKET_OUTCOME_UNKNOWN);
    if(status!=POCKET_STATUS_OK)
        return pocket_api_error(ctx,POCKET_ERR_IO_ERROR,"io.ir.send",
                                "the transmission stopped early",true,
                                POCKET_OUTCOME_UNKNOWN);
    *rejected=false;
    return JS_UNDEFINED;
}

static void ir_released(void *user) { (void)user; ir_busy=false; }

static const pocket_promise_ops_t ir_ops = {
    .settle=ir_settle, .stop=ir_stop, .release=ir_released,
};

static esp_err_t ir_channel_ready(void) {
    if(ir_channel) return ESP_OK;
    rmt_tx_channel_config_t cfg = {
        .gpio_num=IR_GPIO, .clk_src=RMT_CLK_SRC_DEFAULT,
        .resolution_hz=1000000,          // 1us per tick, so durationsUs is literal
        .mem_block_symbols=64, .trans_queue_depth=1,
    };
    esp_err_t e=rmt_new_tx_channel(&cfg,&ir_channel);
    if(e!=ESP_OK) { ir_channel=NULL; return e; }
    rmt_copy_encoder_config_t encoder = {};   // the copy encoder takes none
    e=rmt_new_copy_encoder(&encoder,&ir_encoder);
    if(e!=ESP_OK) {
        rmt_del_channel(ir_channel); ir_channel=NULL; ir_encoder=NULL;
        return e;
    }
    rmt_tx_event_callbacks_t callbacks = { .on_trans_done=ir_done };
    return rmt_tx_register_event_callbacks(ir_channel,&callbacks,NULL);
}

static JSValue ir_send(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    (void)this_val;
    static const char *const OP="io.ir.send";
    if(argc<1 || !JS_IsObject(argv[0]))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "send(spec, options) needs a spec object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    int64_t carrier=0;
    JSValue error=JS_UNDEFINED;
    if(!take_int(ctx,argv[0],"carrierHz",OP,IR_MIN_CARRIER_HZ,IR_MAX_CARRIER_HZ,
                 &carrier,&error)) return error;

    JSValue list=JS_GetPropertyStr(ctx,argv[0],"durationsUs");
    if(JS_IsException(list)) return JS_EXCEPTION;
    if(!JS_IsArray(list)) {
        JS_FreeValue(ctx,list);
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "durationsUs must be an array",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    uint32_t count=0;
    {
        JSValue length=JS_GetPropertyStr(ctx,list,"length");
        if(JS_ToUint32(ctx,&count,length)) count=0;
        JS_FreeValue(ctx,length);
    }
    if(count<1 || count>IR_MAX_DURATIONS) {
        JS_FreeValue(ctx,list);
        return pocket_api_reject(ctx,
            count?POCKET_ERR_LIMIT_EXCEEDED:POCKET_ERR_INVALID_ARGUMENT,OP,
            "durationsUs must hold 1 to maxDurations entries",false,
            POCKET_OUTCOME_NOT_APPLIED);
    }

    // Section 9: mark and space alternate, the first entry is a mark and every
    // entry is positive. The odd entry out is a mark with no space after it,
    // which the encoder still needs a second half for, so it gets the shortest
    // space the hardware can express -- a zero-length half would run forever.
    int64_t total=0;
    if(!ir_symbols) {
        ir_symbols=calloc(IR_MAX_DURATIONS/2,sizeof(*ir_symbols));
        if(!ir_symbols) {
            JS_FreeValue(ctx,list);
            return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                     "no memory for the frame buffer",true,
                                     POCKET_OUTCOME_NOT_APPLIED);
        }
    }
    memset(ir_symbols,0,(IR_MAX_DURATIONS/2)*sizeof(*ir_symbols));
    for(uint32_t i=0;i<count;i++) {
        JSValue item=JS_GetPropertyUint32(ctx,list,i);
        double v=0;
        bool bad=!JS_IsNumber(item) || JS_ToFloat64(ctx,&v,item);
        JS_FreeValue(ctx,item);
        if(bad || !isfinite(v) || v!=(double)(int64_t)v ||
           v<1 || v>IR_MAX_ONE_US) {
            JS_FreeValue(ctx,list);
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                     "each duration must be a whole 1 to 32767 us",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        }
        total+=(int64_t)v;
        rmt_symbol_word_t *symbol=&ir_symbols[i/2];
        if(i%2==0) { symbol->level0=1; symbol->duration0=(uint16_t)v; }
        else       { symbol->level1=0; symbol->duration1=(uint16_t)v; }
    }
    JS_FreeValue(ctx,list);
    if(count%2) { ir_symbols[count/2].level1=0; ir_symbols[count/2].duration1=1; }
    if(total>IR_MAX_TOTAL_US)
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "the frame is longer than maxTotalUs",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    io_options_t options;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,
                             IO_MAX_TIMEOUT_MS,&options);
    if(!JS_IsUndefined(bad)) return bad;
    if(ir_busy) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "a frame is already going out",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(options.cancelled) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the frame",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    esp_err_t e=ir_channel_ready();
    if(e!=ESP_OK) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_NOT_AVAILABLE,OP,
                                 esp_err_to_name(e),false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    // The carrier can only be applied while the channel is disabled, so it is
    // set per frame and the channel is enabled around the transmission alone.
    rmt_carrier_config_t modulation = {
        .frequency_hz=(uint32_t)carrier, .duty_cycle=0.33f,
    };
    e=rmt_apply_carrier(ir_channel,&modulation);
    if(e==ESP_OK) e=rmt_enable(ir_channel);
    if(e!=ESP_OK) {
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,esp_err_to_name(e),
                                 true,POCKET_OUTCOME_NOT_APPLIED);
    }
    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        rmt_disable(ir_channel);
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    ir_request=request;
    size_t symbols=(count+1)/2;
    rmt_transmit_config_t tx = { .loop_count=0 };
    e=rmt_transmit(ir_channel,ir_encoder,ir_symbols,
                   symbols*sizeof(ir_symbols[0]),&tx);
    if(e!=ESP_OK) {
        pocket_api_promise_abandon(request);
        rmt_disable(ir_channel);
        JS_FreeValue(ctx,options.cancel);
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,esp_err_to_name(e),
                                 true,POCKET_OUTCOME_NOT_APPLIED);
    }
    // The whole frame is at most IR_MAX_TOTAL_US, so the default deadline is
    // that plus the same second of slack a queued tone gets.
    int64_t deadline=esp_timer_get_time()+
        (options.timeout_ms?1000LL*options.timeout_ms:IR_MAX_TOTAL_US+1000000LL);
    JSValue promise=pocket_api_promise_arm(ctx,request,&ir_ops,
                                           (void *)(uintptr_t)request,
                                           options.cancel,deadline);
    if(JS_IsException(promise)) {
        rmt_disable(ir_channel);
        return promise;
    }
    ir_busy=true;
    return promise;
}

// --------------------------------------------------------------- dispatch

enum {
    M_I2C_TRANSFER, M_SPI_TRANSFER, M_UART_READ, M_UART_WRITE,
    M_GPIO_READ, M_GPIO_WRITE, M_GPIO_WATCH, M_CLOSE,
};

// close() and the synchronous pin methods throw; everything else rejects. That
// split is section 4's, not a preference: a method that returns a Promise puts
// its argument errors into the rejection.
static bool method_is_async(int magic) {
    return magic!=M_GPIO_READ && magic!=M_GPIO_WRITE && magic!=M_GPIO_WATCH &&
           magic!=M_CLOSE;
}

static JSValue js_method(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv, int magic,
                         JSValueConst *func_data) {
    (void)this_val;
    int32_t id=0;
    JS_ToInt32(ctx,&id,func_data[0]);
    io_handle_t *h=slot_of(id);
    if(!h) {
        static const char *const OP="io.handle";
        if(method_is_async(magic))
            return pocket_api_reject(ctx,POCKET_ERR_CLOSED,OP,
                                     "this handle is closed",false,
                                     POCKET_OUTCOME_NOT_APPLIED);
        // close() on an already closed handle is a no-op: section 4 makes
        // close idempotent.
        if(magic==M_CLOSE) return JS_UNDEFINED;
        return pocket_api_throw(ctx,POCKET_ERR_CLOSED,OP,
                                "this handle is closed",false,NULL);
    }
    switch(magic) {
        case M_I2C_TRANSFER:  return i2c_transfer(ctx,h,argc,argv);
        case M_SPI_TRANSFER:  return spi_transfer(ctx,h,argc,argv);
        case M_UART_READ:     return uart_read(ctx,h,id,argc,argv);
        case M_UART_WRITE:    return uart_write(ctx,h,argc,argv);
        case M_GPIO_WATCH:    return gpio_watch(ctx,h,id,argc,argv);
        case M_GPIO_READ:
            return JS_NewInt32(ctx,gpio_get_level(PORTS[h->port].pins[0]));
        case M_GPIO_WRITE: {
            if(!h->gpio_output)
                return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                                        "io.gpio.write","this pin is an input",
                                        false,NULL);
            int32_t level=0;
            if(argc<1 || !JS_IsNumber(argv[0]) ||
               JS_ToInt32(ctx,&level,argv[0]) || (level!=0 && level!=1))
                return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                                        "io.gpio.write","value must be 0 or 1",
                                        false,NULL);
            gpio_set_level(PORTS[h->port].pins[0],(uint32_t)level);
            return JS_UNDEFINED;
        }
        default:
            handle_close(h);
            return JS_UNDEFINED;
    }
}

static void add_method(JSContext *ctx, JSValue object, const char *name,
                       int length, int magic, JSValueConst id) {
    JS_DefinePropertyValueStr(ctx,object,name,
        JS_NewCFunctionData(ctx,js_method,length,magic,1,&id),
        JS_PROP_ENUMERABLE);
}

// Builds the handle object an open() resolves with. The methods carry the
// handle's identity as bound data, so nothing about the device is reachable
// from JS except through them.
static JSValue handle_object(JSContext *ctx, handle_kind_t kind, int32_t id) {
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) return object;
    JSValue key=JS_NewInt32(ctx,id);
    switch(kind) {
        case H_I2C: add_method(ctx,object,"transfer",2,M_I2C_TRANSFER,key); break;
        case H_SPI: add_method(ctx,object,"transfer",2,M_SPI_TRANSFER,key); break;
        case H_UART:
            add_method(ctx,object,"read",2,M_UART_READ,key);
            add_method(ctx,object,"write",2,M_UART_WRITE,key);
            break;
        default:
            add_method(ctx,object,"read",0,M_GPIO_READ,key);
            add_method(ctx,object,"write",1,M_GPIO_WRITE,key);
            add_method(ctx,object,"watch",2,M_GPIO_WATCH,key);
            break;
    }
    add_method(ctx,object,"close",0,M_CLOSE,key);
    JS_FreeValue(ctx,key);
    return object;
}

// The four open()s differ only in which builder runs and which kind comes out,
// so the promise the app receives is assembled once. Each builder returns
// JS_UNDEFINED after filling a slot, or the rejection it wants handed back.
static JSValue finish_open(JSContext *ctx, JSValue attempt, handle_kind_t kind) {
    int slot=opened_slot;
    opened_slot=-1;
    if(!JS_IsUndefined(attempt)) return attempt;
    if(slot<0 || handles[slot].kind!=kind)
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,"io.open",
                                 "the handle went away during open",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValue device=handle_object(ctx,kind,handle_id(slot));
    if(JS_IsException(device)) { handle_close(&handles[slot]); return device; }
    return pocket_api_settled(ctx,device,false);
}

// --------------------------------------------------------------- io.ports

static JSValue js_ports(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    static const char *const NAMES[]={"i2c","spi","uart","gpio","ir"};
    JSValue array=JS_NewArray(ctx);
    if(JS_IsException(array)) return array;
    for(unsigned i=0;i<PORT_COUNT;i++) {
        JSValue info=JS_NewObject(ctx);
        JS_SetPropertyStr(ctx,info,"id",JS_NewString(ctx,PORTS[i].id));
        JSValue protocols=JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx,protocols,0,
                             JS_NewString(ctx,NAMES[PORTS[i].proto]));
        JS_SetPropertyStr(ctx,info,"protocols",protocols);
        // Every port published here is one a program may open. The pins that
        // are genuinely reserved -- panel, flash, USB, power, strapping, I2S,
        // the SD chip select -- are not in the table at all, which is what
        // section 10 asks for, so this flag is false throughout.
        JS_SetPropertyStr(ctx,info,"reserved",JS_FALSE);
        JS_SetPropertyStr(ctx,info,"available",JS_NewBool(ctx,pins_free((int)i)));
        JSValue limits=JS_NewObject(ctx);
        JS_SetPropertyStr(ctx,limits,"wiring",JS_NewString(ctx,PORTS[i].wiring));
        if(!strcmp(PORTS[i].id,"ext.i2c")) {
            JS_SetPropertyStr(ctx,limits,"sharedWith",
                              JS_NewString(ctx,"keyboard,imu,codec"));
            JS_SetPropertyStr(ctx,limits,"maxWaitMs",
                              JS_NewInt32(ctx,I2C_EXT_WAIT_MS));
        } else if(!strcmp(PORTS[i].id,"grove")) {
            JS_SetPropertyStr(ctx,limits,"maxWaitMs",
                              JS_NewInt32(ctx,I2C_GROVE_WAIT_MS));
        } else if(!strcmp(PORTS[i].id,"ext.spi")) {
            JS_SetPropertyStr(ctx,limits,"sharedWith",JS_NewString(ctx,"microsd"));
        }
        JS_SetPropertyStr(ctx,info,"limits",limits);
        JS_SetPropertyUint32(ctx,array,i,info);
    }
    return array;
}

// --------------------------------------------------------- pump and reset

void pocket_io_pump(void) {
    // One load and a branch when a program uses none of this.
    for(int i=0;i<IO_HANDLES;i++) {
        io_handle_t *h=&handles[i];
        if(h->kind!=H_UART || !h->uart_read) continue;
        size_t waiting=0;
        if(uart_get_buffered_data_len(UART_NUM_1,&waiting)==ESP_OK && waiting)
            pocket_api_complete(h->uart_read,POCKET_STATUS_OK);
    }
    if(watch_table.open) watch_pump();
}

// Whether pocket.io was ever read. Without it no pin was claimed, no bus was
// built and no IR channel exists, so the whole of this is someone else's work.
static bool built;

void pocket_io_reset(void) {
    if(!built) return;
    built=false;
    for(int i=0;i<IO_HANDLES;i++)
        if(handles[i].kind!=H_FREE) handle_close(&handles[i]);
    pocket_api_sub_close_all(&watch_table);
    watch_table.ctx=NULL;
    // The IR frame is not stopped here: it waits on a promise slot, and
    // pocket_api_reset() is what asks it to stop. The channel goes either way,
    // because rmt_del_channel needs the channel disabled and ir_stop has just
    // done that.
    if(ir_channel) { rmt_disable(ir_channel); rmt_del_channel(ir_channel); }
    if(ir_encoder) rmt_del_encoder(ir_encoder);
    ir_channel=NULL; ir_encoder=NULL; ir_busy=false;
    // After the channel is gone, so nothing can still be reading the frame.
    free(ir_symbols);
    ir_symbols=NULL;
    // handle_close() above dropped the last grove user, but an open() that
    // built the bus and then failed to add its device left one with no users.
    if(grove_bus) { i2c_del_master_bus(grove_bus); grove_bus=NULL; }
    grove_users=0;
    claimed_pins=0;
}

// ------------------------------------------------------------ capabilities
//
// Section 2's rule for limits: what the code enforces, not what the document
// proposes. Where the two differ -- the shared bus's 50ms wait against section
// 14's 100, SPI's 20ms hold ceiling, a polled GPIO edge instead of an
// interrupt -- the number here is this file's.

static const pocket_limit_t i2c_limits[] = {
    {.name="maxTransferBytes",  .kind=POCKET_LIMIT_INT, .number=I2C_MAX_BYTES},
    {.name="maxWaitMs",         .kind=POCKET_LIMIT_INT, .number=I2C_EXT_WAIT_MS},
    {.name="groveMaxWaitMs",    .kind=POCKET_LIMIT_INT, .number=I2C_GROVE_WAIT_MS},
    {.name="speeds",            .kind=POCKET_LIMIT_TEXT,.text="100000,400000"},
    {.name="reservedAddresses", .kind=POCKET_LIMIT_TEXT,
     .text="0x00-0x07,0x18,0x34,0x68,0x69,0x78-0x7f"},
    {.name="scan",              .kind=POCKET_LIMIT_FLAG,.number=0},
    {.name="maxHandles",        .kind=POCKET_LIMIT_INT, .number=IO_HANDLES},
    {0},
};
static const pocket_limit_t spi_limits[] = {
    {.name="maxTransferBytes",.kind=POCKET_LIMIT_INT, .number=SPI_MAX_BYTES},
    {.name="minHz",           .kind=POCKET_LIMIT_INT, .number=SPI_MIN_HZ},
    {.name="maxHz",           .kind=POCKET_LIMIT_INT, .number=SPI_MAX_HZ},
    {.name="maxHoldMs",       .kind=POCKET_LIMIT_INT, .number=SPI_MAX_HOLD_US/1000},
    {.name="csHeldAcrossCalls",.kind=POCKET_LIMIT_FLAG,.number=0},
    {.name="maxHandles",      .kind=POCKET_LIMIT_INT, .number=1},
    {0},
};
static const pocket_limit_t uart_limits[] = {
    {.name="maxReadBytes",  .kind=POCKET_LIMIT_INT, .number=UART_MAX_READ},
    {.name="maxWriteBytes", .kind=POCKET_LIMIT_INT, .number=UART_MAX_WRITE},
    {.name="rxRingBytes",   .kind=POCKET_LIMIT_INT, .number=UART_RX_RING},
    {.name="minBaud",       .kind=POCKET_LIMIT_INT, .number=UART_MIN_BAUD},
    {.name="maxBaud",       .kind=POCKET_LIMIT_INT, .number=UART_MAX_BAUD},
    {.name="readTimeoutMs", .kind=POCKET_LIMIT_INT, .number=UART_READ_TIMEOUT},
    {.name="framing",       .kind=POCKET_LIMIT_TEXT,.text="8N1"},
    {.name="maxHandles",    .kind=POCKET_LIMIT_INT, .number=1},
    {0},
};
static const pocket_limit_t gpio_limits[] = {
    {.name="maxWatches",     .kind=POCKET_LIMIT_INT, .number=IO_GPIO_WATCHES},
    {.name="maxDebounceMs",  .kind=POCKET_LIMIT_INT, .number=GPIO_MAX_DEBOUNCE},
    // Sampled once a frame from the JS task, so a pulse shorter than this is
    // not seen at all. Published rather than implied.
    {.name="pollMs",         .kind=POCKET_LIMIT_INT, .number=33},
    {.name="edgeDetection",  .kind=POCKET_LIMIT_TEXT,.text="polled"},
    {.name="closeState",     .kind=POCKET_LIMIT_TEXT,.text="input-floating"},
    {0},
};
static const pocket_limit_t ir_limits[] = {
    {.name="maxDurations",  .kind=POCKET_LIMIT_INT, .number=IR_MAX_DURATIONS},
    {.name="maxTotalUs",    .kind=POCKET_LIMIT_INT, .number=IR_MAX_TOTAL_US},
    {.name="maxDurationUs", .kind=POCKET_LIMIT_INT, .number=IR_MAX_ONE_US},
    {.name="minCarrierHz",  .kind=POCKET_LIMIT_INT, .number=IR_MIN_CARRIER_HZ},
    {.name="maxCarrierHz",  .kind=POCKET_LIMIT_INT, .number=IR_MAX_CARRIER_HZ},
    {.name="dutyCycle",     .kind=POCKET_LIMIT_TEXT,.text="0.33"},
    {.name="receive",       .kind=POCKET_LIMIT_FLAG,.number=0},
    {.name="concurrent",    .kind=POCKET_LIMIT_INT, .number=1},
    {0},
};

// available is "the wiring is there and nothing else has it", which for these
// is a question about handles rather than about a device answering: section 2
// calls available an observation, and open() is the final word.
static void handles_probe(const pocket_capability_t *cap, bool *available,
                          const char **reason) {
    (void)cap;
    *available=free_slot()>=0;
    *reason=*available?NULL:POCKET_REASON_BUSY;
}

static const pocket_capability_t io_i2c_capability = {
    .name="io.i2c", .supported=true, .available=true, .limits=i2c_limits,
    .probe=handles_probe,
};
static const pocket_capability_t io_spi_capability = {
    .name="io.spi", .supported=true, .available=true, .limits=spi_limits,
    .probe=handles_probe,
};
static const pocket_capability_t io_uart_capability = {
    .name="io.uart", .supported=true, .available=true, .limits=uart_limits,
    .probe=handles_probe,
};
static const pocket_capability_t io_gpio_capability = {
    .name="io.gpio", .supported=true, .available=true, .limits=gpio_limits,
    .probe=handles_probe,
};
// available=true with no probe: the emitter is soldered to GPIO44 and there is
// nothing to ask. What cannot be observed is whether the light comes out, and
// no probe here would be measuring that -- so none pretends to.
static const pocket_capability_t io_ir_capability = {
    .name="io.ir", .supported=true, .available=true, .limits=ir_limits,
};

// ------------------------------------------------------------------ install

static JSValue js_i2c_open(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    return finish_open(ctx,i2c_open(ctx,this_val,argc,argv),H_I2C);
}
static JSValue js_spi_open(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    return finish_open(ctx,spi_open(ctx,this_val,argc,argv),H_SPI);
}
static JSValue js_uart_open(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    return finish_open(ctx,uart_open(ctx,this_val,argc,argv),H_UART);
}
static JSValue js_gpio_open(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv) {
    return finish_open(ctx,gpio_open(ctx,this_val,argc,argv),H_GPIO);
}

static void add_open(JSContext *ctx, JSValue parent, const char *child,
                     JSCFunction *open) {
    JSValue object=JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx,object,"open",
        JS_NewCFunction(ctx,open,"open",2),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,parent,child,object,JS_PROP_ENUMERABLE);
}

static esp_err_t build_io(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    // A realm going away takes its callbacks with it, so the tables start empty
    // every time this is built. Nothing native can still be open here:
    // app_stop() runs pocket_io_reset() before the guest goes.
    for(int i=0;i<IO_HANDLES;i++) {
        handles[i].kind=H_FREE;
        handles[i].gpio_watch=-1;
        handles[i].uart_read=0;
    }
    for(int i=0;i<IO_GPIO_WATCHES;i++) {
        watch_slots[i].callback=JS_UNDEFINED;
        watch_slots[i].handle=0;
        watch_state[i].id=0;
    }
    watch_table.open=0;
    watch_table.ctx=ctx;
    claimed_pins=0;

    JS_DefinePropertyValueStr(ctx,ns,"ports",
        JS_NewCFunction(ctx,js_ports,"ports",0),JS_PROP_ENUMERABLE);
    add_open(ctx,ns,"i2c",js_i2c_open);
    add_open(ctx,ns,"spi",js_spi_open);
    add_open(ctx,ns,"uart",js_uart_open);
    add_open(ctx,ns,"gpio",js_gpio_open);

    JSValue ir=JS_NewObject(ctx);
    if(JS_IsException(ir)) return ESP_ERR_NO_MEM;
    JS_DefinePropertyValueStr(ctx,ir,"send",
        JS_NewCFunction(ctx,ir_send,"send",2),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"ir",ir,JS_PROP_ENUMERABLE);

    built=true;
    ESP_LOGI(TAG,"pocket.io ready: %u ports, %d handles",
             (unsigned)PORT_COUNT,IO_HANDLES);
    return ESP_OK;
}

esp_err_t pocket_io_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    // Five capability entries, eager, so a feature test for io.spi answers
    // without the other four surfaces being built to ask.
    pocket_api_register(&io_i2c_capability);
    pocket_api_register(&io_spi_capability);
    pocket_api_register(&io_uart_capability);
    pocket_api_register(&io_gpio_capability);
    pocket_api_register(&io_ir_capability);
    return pocket_api_lazy(ctx,"io",build_io,NULL);
}
