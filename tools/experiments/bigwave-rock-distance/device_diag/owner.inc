/* Included in a generated main.c; no new task, control endpoint or USB use. */
extern bool rock_diag_finished(void);
extern bool rock_diag_valid(void);
extern void rock_diag_owner_tick(unsigned,bool);
extern void rock_diag_finish(const char *);
static bool diag_owner_active,diag_owner_ran;
static int64_t diag_owner_deadline;
static const char diag_prelude[]=
    "(()=>{let n=0;const log=console.log;console.log=(...a)=>{if(n++<16)log(...a)}})();";
static void diag_owner_step(void){
    if(!diag_owner_ran){
        diag_owner_ran=true;diag_owner_active=true;
        diag_owner_deadline=esp_timer_get_time()+24000000;
        printf("BW_ROCK_DIAG START workload=@WORKLOAD@ source=@SOURCE@ allocator=GUEST_ALLOCATOR owner=ui_task lcd=product n=120 deadline_ms=24000 setup_frames=82\n");
        begin_run("local.bigwave",diag_prelude,sizeof(diag_prelude)-1,big_wave_start,(size_t)(big_wave_end-big_wave_start-1));
    }
    const int64_t diag_now=esp_timer_get_time();
    if(diag_owner_active&&(rock_diag_finished()||!running||diag_now>=diag_owner_deadline)){
        const char *status=!running?"app_error":diag_now>=diag_owner_deadline?"timeout":!rock_diag_finished()?"timeout":
            !rock_diag_valid()?"input_mismatch":pocket_kasane_needs_present()?"present_pending":"complete";
        rock_diag_finish(status);
        if(running)end_run(ESP_OK);
        diag_owner_active=false;
        printf("BW_ROCK_DIAG EXIT running=%d\n",(int)running);
    }
}
