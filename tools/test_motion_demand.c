// Host check of motion.c's reader demand: no reader, no I2C; the home lease;
// no stale sample after idle; an idle gap is not counted as dropped; the
// gyroscope request is applied while idle. Build (WSL or Linux):
//   gcc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -Itools/motion_shim
//    -Itools/hostshim -Ithird_party/bmi270 -Imain/hal tools/test_motion_demand.c -lm -o /tmp/tmd && /tmp/tmd
#undef NDEBUG
#include <assert.h>
#include <stddef.h>
#include "../main/hal/motion.c"
static int64_t clock_us=1000000; static int reads, gyro_on;
int64_t esp_timer_get_time(void){return clock_us;}
esp_err_t i2c_master_probe(i2c_master_bus_handle_t b,uint16_t a,int t){(void)b;(void)t;return a==0x68?ESP_OK:ESP_FAIL;}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b,const i2c_device_config_t*c,i2c_master_dev_handle_t*d){(void)b;(void)c;*d=(void*)1;return ESP_OK;}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d,const uint8_t*w,size_t wl,uint8_t*r,size_t rl,int t){(void)d;(void)w;(void)wl;(void)r;(void)rl;(void)t;return ESP_OK;}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d,const uint8_t*w,size_t wl,int t){(void)d;(void)w;(void)wl;(void)t;return ESP_OK;}
int8_t bmi270_init(struct bmi2_dev*d){d->chip_id=0x24;return 0;}
int8_t bmi2_get_sensor_config(struct bmi2_sens_config*c,uint8_t n,struct bmi2_dev*d){(void)c;(void)n;(void)d;return 0;}
int8_t bmi2_set_sensor_config(struct bmi2_sens_config*c,uint8_t n,struct bmi2_dev*d){(void)c;(void)n;(void)d;return 0;}
int8_t bmi2_sensor_enable(const uint8_t*l,uint8_t n,struct bmi2_dev*d){(void)n;(void)d;if(*l==BMI2_GYRO)gyro_on=1;return 0;}
int8_t bmi2_sensor_disable(const uint8_t*l,uint8_t n,struct bmi2_dev*d){(void)n;(void)d;if(*l==BMI2_GYRO)gyro_on=0;return 0;}
int8_t bmi2_get_sensor_data(struct bmi2_sens_data*s,struct bmi2_dev*d){(void)d;reads++;s->status=BMI2_DRDY_ACC;s->acc.z=16384;return 0;}
static void step(int ms){for(int i=0;i<ms/5;i++){clock_us+=5000;motion_poll();}}
int main(void){
    motion_sample_t s;
    motion_init((i2c_master_bus_handle_t)1);
    step(100); assert(reads==0 && !motion_latest(&s));          // nobody asked: no I2C
    int x,y; motion_get(&x,&y); step(100); assert(reads==5);     // home lease, 50 Hz
    assert(motion_latest(&s) && s.sequence==5);
    step(300); assert(reads<=5+13 && !motion_latest(&s));       // lease ran out: idle, no stale sample
    int idle_reads=reads; step(500); assert(reads==idle_reads);
    motion_want_app(true); step(20); assert(motion_latest(&s) && s.dropped==0); // gap is not "dropped"
    motion_request_gyro(true); step(20); assert(gyro_on);
    motion_want_app(false); motion_request_gyro(false); step(20); assert(!gyro_on); // applied while idle
    int r=reads; step(1000); assert(reads==r);
    puts("MOTION_DEMAND PASS");
    return 0;
}
