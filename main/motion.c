#include "motion.h"
#include "bmi270.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>

static i2c_master_dev_handle_t device;
static struct bmi2_dev imu;
static bool ready, centered;
static float origin_x,origin_y,filtered_x,filtered_y;
static atomic_int tilt_x,tilt_y;
static int64_t last_read,last_log;
static unsigned failures;
static int8_t read_reg(uint8_t reg,uint8_t *data,uint32_t len,void *ctx) {
    (void)ctx;return i2c_master_transmit_receive(device,&reg,1,data,len,10)==ESP_OK?0:-1;
}
static int8_t write_reg(uint8_t reg,const uint8_t *data,uint32_t len,void *ctx) {
    (void)ctx;uint8_t buffer[65];if(len>64)return -1;
    buffer[0]=reg;memcpy(buffer+1,data,len);
    return i2c_master_transmit(device,buffer,len+1,30)==ESP_OK?0:-1;
}
static void delay_us(uint32_t us,void *ctx) {
    (void)ctx;if(us>=2000)vTaskDelay(pdMS_TO_TICKS((us+999)/1000));else esp_rom_delay_us(us);
}
void motion_init(i2c_master_bus_handle_t bus) {
    int addr=0;
    for(int a=0x68;a<=0x69;a++)if(i2c_master_probe(bus,a,20)==ESP_OK){addr=a;break;}
    if(!addr){ESP_LOGW("motion","BMI270 absent; static parallax");return;}
    i2c_device_config_t cfg={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=addr,.scl_speed_hz=400000};
    if(i2c_master_bus_add_device(bus,&cfg,&device)!=ESP_OK)return;
    imu.intf=BMI2_I2C_INTF;imu.read=read_reg;imu.write=write_reg;imu.delay_us=delay_us;imu.read_write_len=32;
    int rc=bmi270_init(&imu);
    struct bmi2_sens_config sensor={.type=BMI2_ACCEL};
    if(!rc)rc=bmi2_get_sensor_config(&sensor,1,&imu);
    sensor.cfg.acc.odr=BMI2_ACC_ODR_50HZ;sensor.cfg.acc.range=BMI2_ACC_RANGE_2G;
    if(!rc)rc=bmi2_set_sensor_config(&sensor,1,&imu);
    uint8_t accel=BMI2_ACCEL;
    if(!rc)rc=bmi2_sensor_enable(&accel,1,&imu);
    ready=rc==0;
    ESP_LOGI("motion","BMI270 addr=0x%x chip=0x%x init=%d accel=50Hz",addr,imu.chip_id,rc);
}
void motion_recenter(void){centered=false;}
void motion_get(int *x,int *y){*x=atomic_load(&tilt_x);*y=atomic_load(&tilt_y);}
void motion_poll(void) {
    int64_t now=esp_timer_get_time();if(!ready||now-last_read<20000)return;last_read=now;
    struct bmi2_sens_data data={0};
    if(bmi2_get_sensor_data(&data,&imu)!=BMI2_OK){
        if(++failures>=10){atomic_store(&tilt_x,0);atomic_store(&tilt_y,0);}
        return;
    }
    failures=0;if(!(data.status&BMI2_DRDY_ACC))return;
    float x=data.acc.x/16384.0f,y=data.acc.y/16384.0f;
    if(!centered){origin_x=x;origin_y=y;filtered_x=0;filtered_y=0;centered=true;}
    filtered_x+=(x-origin_x-filtered_x)*0.10f;filtered_y+=(y-origin_y-filtered_y)*0.10f;
    int tx=(int)(filtered_x*256),ty=(int)(filtered_y*256);
    if(tx>180)tx=180;
    if(tx< -180)tx=-180;
    if(ty>180)ty=180;
    if(ty< -180)ty=-180;
    atomic_store(&tilt_x,tx);atomic_store(&tilt_y,ty);
    if(now-last_log>5000000){last_log=now;ESP_LOGI("motion","ACC %d %d %d TILT %d %d",data.acc.x,data.acc.y,data.acc.z,tx,ty);}
}
