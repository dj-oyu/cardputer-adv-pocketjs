#include "motion.h"
#include "bmi270.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>
#include <math.h>

static i2c_master_dev_handle_t device;
static struct bmi2_dev imu;
static bool ready, centered;
static float origin_y,filtered_x,filtered_y;
static atomic_int tilt_x,tilt_y;
static int64_t last_read,last_log;
static unsigned failures;

// Full-scale conversions taken from the BMI270 datasheet, not measured here.
// The accelerometer is configured for +-2 g over a signed 16-bit word, so 16384
// LSB/g, which is where the existing /16384 below comes from. The gyroscope is
// configured for +-2000 deg/s: the widest range, so that a flick of the wrist
// clips nothing, and still 0.061 deg/s per count, far below the part's noise.
#define GRAVITY           9.80665f
#define GYRO_RADS_PER_LSB (2000.0f*3.14159265f/180.0f/32768.0f)
#define SAMPLE_PERIOD_US  20000

// Board axes to the published frame (x right, y up, z toward the viewer).
//
// UNVERIFIED. How Bosch's package sits on the ADV board is in no document we
// have, and no amount of software can find out, so this is the identity
// permutation until the hardware says otherwise. The periodic log at the end of
// motion_poll prints the mapped values in m/s^2 precisely so the six-position
// check in acceptance condition 7 can be read straight off the monitor. Change
// only these three lines afterwards: everything downstream reads what they
// produce, including roll and pitch.
#define MAP_X(ax,ay,az) (ax)
#define MAP_Y(ax,ay,az) (ay)
#define MAP_Z(ax,ay,az) (az)

// motion_poll writes from input_task; motion_latest reads from whichever task
// the caller runs on. An odd version means a write is in progress, so a reader
// that sees the same even version either side of its copy knows the copy held
// together. Cheaper on the writing side than a mutex, and that is the side that
// runs every 20 ms.
static motion_sample_t latest;
static atomic_uint version;
static uint32_t sequence,dropped;
static int64_t last_sample;
static atomic_bool gyro_wanted;
static bool gyro_running;

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
    // The gyroscope is configured here and enabled only on request, so turning
    // it on later costs one register write instead of a reconfiguration.
    struct bmi2_sens_config sensor[2]={{.type=BMI2_ACCEL},{.type=BMI2_GYRO}};
    if(!rc)rc=bmi2_get_sensor_config(sensor,2,&imu);
    sensor[0].cfg.acc.odr=BMI2_ACC_ODR_50HZ;sensor[0].cfg.acc.range=BMI2_ACC_RANGE_2G;
    sensor[1].cfg.gyr.odr=BMI2_GYR_ODR_50HZ;sensor[1].cfg.gyr.range=BMI2_GYR_RANGE_2000;
    sensor[1].cfg.gyr.ois_range=BMI2_GYR_OIS_2000;
    sensor[1].cfg.gyr.filter_perf=BMI2_PERF_OPT_MODE;
    sensor[1].cfg.gyr.noise_perf=BMI2_POWER_OPT_MODE;
    if(!rc)rc=bmi2_set_sensor_config(sensor,2,&imu);
    uint8_t accel=BMI2_ACCEL;
    if(!rc)rc=bmi2_sensor_enable(&accel,1,&imu);
    ready=rc==0;
    ESP_LOGI("motion","BMI270 addr=0x%x chip=0x%x init=%d accel=50Hz gyro=configured,off",addr,imu.chip_id,rc);
}
void motion_recenter(void){centered=false;}
void motion_get(int *x,int *y){*x=atomic_load(&tilt_x);*y=atomic_load(&tilt_y);}
bool motion_present(void){return ready;}
unsigned motion_rate_hz(void){return 1000000u/SAMPLE_PERIOD_US;}
void motion_request_gyro(bool on){atomic_store(&gyro_wanted,on);}
bool motion_latest(motion_sample_t *out) {
    if(!ready)return false;
    for(int attempt=0;attempt<8;attempt++) {
        unsigned before=atomic_load(&version);
        if(before&1u)continue;
        *out=latest;
        if(atomic_load(&version)==before)return before!=0;
    }
    return false;
}
// Brings the gyroscope's power state in line with the last request. Called from
// motion_poll so that input_task stays the only task touching this device.
static void apply_gyro_request(void) {
    bool want=atomic_load(&gyro_wanted);
    if(want==gyro_running)return;
    uint8_t gyro=BMI2_GYRO;
    int rc=want?bmi2_sensor_enable(&gyro,1,&imu):bmi2_sensor_disable(&gyro,1,&imu);
    if(rc!=BMI2_OK) {
        ESP_LOGW("motion","gyro %s failed: %d",want?"enable":"disable",rc);
        atomic_store(&gyro_wanted,gyro_running);
        return;
    }
    gyro_running=want;
    ESP_LOGI("motion","gyro %s",want?"on":"off");
}
void motion_poll(void) {
    int64_t now=esp_timer_get_time();if(!ready||now-last_read<SAMPLE_PERIOD_US)return;last_read=now;
    apply_gyro_request();
    struct bmi2_sens_data data={0};
    if(bmi2_get_sensor_data(&data,&imu)!=BMI2_OK){
        if(++failures>=10){atomic_store(&tilt_x,0);atomic_store(&tilt_y,0);}
        return;
    }
    failures=0;if(!(data.status&BMI2_DRDY_ACC))return;
    float ax=data.acc.x/16384.0f,ay=data.acc.y/16384.0f,az=data.acc.z/16384.0f;
    // Absolute lateral inclination: booting while tilted must not redefine level.
    float x=atan2f(ax,sqrtf(ay*ay+az*az));
    float y=atan2f(ay,sqrtf(ax*ax+az*az));
    if(!centered){origin_y=y;filtered_x=x;filtered_y=0;centered=true;}
    filtered_x+=(x-filtered_x)*0.10f;filtered_y+=(y-origin_y-filtered_y)*0.10f;
    int tx=(int)(filtered_x*256),ty=(int)(filtered_y*256);
    if(tx>180)tx=180;
    if(tx< -180)tx=-180;
    if(ty>180)ty=180;
    if(ty< -180)ty=-180;
    atomic_store(&tilt_x,tx);atomic_store(&tilt_y,ty);

    // The published sample. Whatever the wallpaper does with the raw board axes
    // above, this half is the one that owes the spec its frame and its units,
    // so it is derived separately rather than scaled out of the tilt filter.
    motion_sample_t sample={0};
    sample.accel_x=MAP_X(ax,ay,az)*GRAVITY;
    sample.accel_y=MAP_Y(ax,ay,az)*GRAVITY;
    sample.accel_z=MAP_Z(ax,ay,az)*GRAVITY;
    if(gyro_running&&(data.status&BMI2_DRDY_GYR)) {
        float gx=data.gyr.x*GYRO_RADS_PER_LSB;
        float gy=data.gyr.y*GYRO_RADS_PER_LSB;
        float gz=data.gyr.z*GYRO_RADS_PER_LSB;
        sample.gyro_x=MAP_X(gx,gy,gz);
        sample.gyro_y=MAP_Y(gx,gy,gz);
        sample.gyro_z=MAP_Z(gx,gy,gz);
        sample.gyro_valid=true;
    }
    sample.roll=atan2f(sample.accel_y,sample.accel_z);
    sample.pitch=atan2f(-sample.accel_x,
        sqrtf(sample.accel_y*sample.accel_y+sample.accel_z*sample.accel_z));
    // An estimate, and labelled as one: a gap longer than one polling period
    // means samples the BMI270 produced that nobody collected. It cannot tell a
    // late poll from a stalled sensor, and it does not use the part's own
    // sensortime, which wraps every 655 ms and so cannot survive a long gap.
    if(last_sample) {
        int missed=(int)((now-last_sample+SAMPLE_PERIOD_US/2)/SAMPLE_PERIOD_US)-1;
        if(missed>0)dropped+=(unsigned)missed;
    }
    last_sample=now;
    sample.time_us=now;sample.sequence=++sequence;sample.dropped=dropped;
    atomic_fetch_add(&version,1u);
    latest=sample;
    atomic_fetch_add(&version,1u);

    // Every two seconds, in the units section 8 publishes, because the axis
    // check in acceptance condition 7 is read off this line: hold the device in
    // each of six orientations and see which component sits near +-9810 while
    // the other two sit near zero. Scaled to integers rather than printed as
    // floats to keep printf's float formatting, and the libm it drags in, out
    // of a path that runs whether anyone is reading the log or not.
    if(now-last_log>2000000){last_log=now;
        ESP_LOGI("motion","ACC %d %d %d mm/s2 GYR %d %d %d mrad/s%s seq=%lu drop=%lu TILT %d %d",
                 (int)(sample.accel_x*1000),(int)(sample.accel_y*1000),(int)(sample.accel_z*1000),
                 (int)(sample.gyro_x*1000),(int)(sample.gyro_y*1000),(int)(sample.gyro_z*1000),
                 sample.gyro_valid?"":" (off)",
                 (unsigned long)sample.sequence,(unsigned long)sample.dropped,tx,ty);}
}
