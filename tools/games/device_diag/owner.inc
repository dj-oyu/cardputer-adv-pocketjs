/* Included in a generated main.c; no new task, control endpoint or USB use. */
extern bool derby_diag_finished(void);
extern void derby_diag_owner_tick(unsigned,bool);
extern void derby_diag_finish(const char *);
static bool diag_owner_active,diag_owner_ran;
static int64_t diag_owner_deadline;
static const char diag_prelude[]=
    "pocket.random.seed=()=>0x2545f491;"
    "pocket.storage.get=()=>Promise.resolve({value:null,revision:0});"
    "pocket.storage.set=()=>Promise.resolve();"
    "(()=>{let n=0;const log=console.log;console.log=(...a)=>{if(n++<16)log(...a)}})();";
static void diag_owner_step(void){
    if(!diag_owner_ran){
        diag_owner_ran=true;diag_owner_active=true;
        diag_owner_deadline=esp_timer_get_time()+24000000;
        printf("DERBY_APP_DIAG START workload=@WORKLOAD@ source=@SOURCE@ allocator=GUEST_ALLOCATOR owner=ui_task lcd=product n=120 deadline_ms=24000\n");
        begin_run("local.derby",diag_prelude,sizeof(diag_prelude)-1,derby_watch_start,(size_t)(derby_watch_end-derby_watch_start-1));
    }
    const int64_t diag_now=esp_timer_get_time();
    if(diag_owner_active&&(derby_diag_finished()||!running||diag_now>=diag_owner_deadline)){
        const char *status=!running?"app_error":diag_now>=diag_owner_deadline?"timeout":!derby_diag_finished()?"timeout":
            pocket_kasane_needs_present()?"present_pending":"complete";
        derby_diag_finish(status);
        if(running)end_run(ESP_OK);
        diag_owner_active=false;
        printf("DERBY_APP_DIAG EXIT running=%d\n",(int)running);
    }
}
