/* Loads the real apps/imucal/imucal.js against the real QuickJS + Kasane
 * fixture, with pocket.* stubbed in JS. Only exercises the NO_IMU path (no
 * hardware in this harness): confirms the Kasane port parses, builds its
 * scene without throwing, and presents -- the real risk in this port was
 * scene/text argument mistakes, not the sensor math (unchanged from the
 * legacy version and covered by reading the source). */
#define main kasane_imucal_test_main
#include "test_pocket_kasane.c"
#undef main

int main(void) {
    check(open_fault_runtime(
        "globalThis.console={log(){}};"
        "globalThis.pocket={kasane:kasane,"
        "capabilities:{get(name){return {supported:true,available:false,reason:'NO_DEVICE'};}}};"),
        "imucal fixture opens with NO_IMU capability");

    FILE *file=fopen("apps/imucal/imucal.js","rb");
    if(!file) { printf("FAIL cannot open apps/imucal/imucal.js\n"); return 2; }
    char source[8192];
    size_t bytes=fread(source,1,sizeof(source)-1,file);
    fclose(file);
    source[bytes]=0;

    check(run(source),"imucal.js runs to completion under NO_IMU capability");
    ksn_render_stats stats;
    check(present(&stats)==KSN_OK,"imucal NO_IMU scene presents");
    check(run("frame();frame();frame();"),"imucal NO_IMU frame() pump does not throw");
    check(present(&stats)==KSN_OK,"imucal NO_IMU repeated present stays OK");
    check(run("if(typeof ui!=='undefined')throw Error('legacy ui global leaked in in scope');"),
          "no legacy ui global observed");

    /* Second scenario: a full six-orientation walk with sensors.imu, storage
     * and audio mocked in JS, driving the exact success path the device runs. */
    check(open_fault_runtime(
        "globalThis.__logs=[];globalThis.console={log(m){__logs.push(String(m));}};"
        "globalThis.__store={};globalThis.__imuCb=null;"
        "globalThis.pocket={kasane:kasane,"
        "capabilities:{get(n){return {supported:true,available:true,reason:null};}},"
        "sensors:{imu:{latest(){return null;},"
        "watch(o,cb){globalThis.__imuCb=cb;return {close(){globalThis.__imuCb=null;}};}}},"
        "storage:{get(k){return new Promise((res,rej)=>{if(k in __store)res(__store[k]);"
        "else rej({code:'NOT_FOUND'});});},"
        "set(k,v){return new Promise((res)=>{__store[k]=v;res();});}},"
        "audio:{tone(){return Promise.resolve();}}};"
        "function setAccel(x,y,z){__imuCb({accel:{x:x,y:y,z:z},gyro:{x:0.01,y:0,z:0},dropped:0});}"
        "function hold(x,y,z){for(let i=0;i<12;i++)setAccel(x,y,z);"
        "for(let i=0;i<50;i++){setAccel(x,y,z);frame();}}"
        "function moveAway(){for(let i=0;i<15;i++){"
        "setAccel((i%2?1:-1)*3,(i%3?1:-1)*3,(i%5?1:-1)*3);frame();}}"),
        "imucal fixture opens with available IMU");
    check(run(source),"imucal.js runs to completion with IMU available");
    check(present(&stats)==KSN_OK,"imucal available-IMU initial scene presents");
    check(run(
        "hold(0,0,9.8);moveAway();"
        "hold(0,0,-9.8);moveAway();"
        "hold(0,9.8,0);moveAway();"
        "hold(0,-9.8,0);moveAway();"
        "hold(9.8,0,0);moveAway();"
        "hold(-9.8,0,0);"),
        "imucal completes the six-orientation walk without throwing");
    check(present(&stats)==KSN_OK,"imucal report scene presents");
    check(run("frame();frame();frame();frame();frame();frame();frame();frame();frame();frame();frame();"),
        "imucal post-report frame() pump (GYRO AFTER check) does not throw");
    check(present(&stats)==KSN_OK,"imucal post-report present stays OK");
    check(run("if(__logs.indexOf('IMUCAL_OK')===-1)"
              "throw Error('missing IMUCAL_OK: '+__logs.join('|'));"),
          "six consistent orientations report IMUCAL_OK");
    check(run("if(__logs.indexOf('IMUCAL_MAP #define MAP_X(ax,ay,az) (-ax)')===-1)"
              "throw Error('missing MAP_X: '+__logs.join('|'));"),
          "IMUCAL_MAP for X matches the driven orientations");
    check(run("if(!('axes' in __store))throw Error('report did not persist to pocket.storage');"),
          "report() saved the result through pocket.storage");

    printf("%s\n",failures?"IMUCAL KASANE TEST FAIL":"IMUCAL KASANE TEST PASS");
    return failures?1:0;
}
