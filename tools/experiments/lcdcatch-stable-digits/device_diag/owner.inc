/* Included in a generated main.c; no new task, control endpoint or USB use. */
extern bool lcd_diag_finished(void);
extern bool lcd_diag_valid(void);
extern void lcd_diag_owner_tick(unsigned,bool);
extern void lcd_diag_finish(const char *);
static bool diag_owner_active,diag_owner_ran;
static int64_t diag_owner_deadline;
static const char diag_prelude[]=
    "pocket.random.seed=()=>20260929;"
    "pocket.storage.get=()=>Promise.resolve({value:null,revision:0});"
    "pocket.storage.set=()=>Promise.resolve();"
    "(()=>{let n=0;const log=console.log;console.log=(...a)=>{if(n++<16)log(...a)}})();";
static void diag_owner_step(void){
    if(!diag_owner_ran){
        diag_owner_ran=true;diag_owner_active=true;
        diag_owner_deadline=esp_timer_get_time()+24000000;
        printf("LCD7_DIAG START workload=@WORKLOAD@ source=@SOURCE@ allocator=GUEST_ALLOCATOR owner=ui_task lcd=product n=120 deadline_ms=24000 setup_frames=73\n");
        begin_run("local.lcdcatch",diag_prelude,sizeof(diag_prelude)-1,lcd_catch_start,(size_t)(lcd_catch_end-lcd_catch_start-1));
    }
    const int64_t diag_now=esp_timer_get_time();
    if(diag_owner_active&&(lcd_diag_finished()||!running||diag_now>=diag_owner_deadline)){
        const char *status=!running?"app_error":diag_now>=diag_owner_deadline?"timeout":!lcd_diag_finished()?"timeout":
            !lcd_diag_valid()?"input_mismatch":pocket_kasane_needs_present()?"present_pending":"complete";
        lcd_diag_finish(status);
        if(running)end_run(ESP_OK);
        diag_owner_active=false;
        printf("LCD7_DIAG EXIT running=%d\n",(int)running);
    }
}
