/* Per-frame tail of the module: system notice update, source wait/advance,
 * invalidate, present (both the KASANE_DMA_OVERLAP split and the single-call
 * form), end/park turn and input scope. It is an .inc, not a .c, because
 * pocket_kasane.c is one translation unit: these functions use the file-static
 * state and parsers directly and the `pocket_kasane:` object name in
 * main/vm/helper_iram8.lf must keep matching. Function order is the original
 * order on purpose. Included only from pocket_kasane.c. */
ksn_result pocket_kasane_update_notice(const sys_notice *notice,uint16_t variant){
    if(dormant||!state||!state->active)return KSN_OK;
    ksn_view *system=ksn_runtime_app_system_view(state->lease);
    if(!system)return KSN_BUSY;
    if(state->notice_tx.value){
        ksn_submission outcome=ksn_view_poll(system);
        if(outcome.ticket.value!=state->notice_tx.value)return KSN_STALE;
        if(outcome.status==KSN_SUBMITTED)return KSN_BUSY;
        if(outcome.status==KSN_PRESENTED){
            state->notice_displayed=state->notice_pending;
            state->notice_variant=state->notice_pending_variant;
        }
        state->notice_tx=(ksn_tx){0};
    }
    uint32_t id=notice?notice->id:0;
    if(id==state->notice_displayed&&(!id||variant==state->notice_variant))return KSN_OK;
    if(notice&&!state->notice_resource.value){
        ksn_image_port image;ksn_result result=ksn_pet_builtin_image(&image);
        if(result==KSN_OK)result=ksn_view_host_register_image(system,&image,&state->notice_resource);
        if(result!=KSN_OK)return result;
    }
    ksn_tx tx;ksn_result result=ksn_view_begin(system,KSN_REPLACE,&tx);
    if(result!=KSN_OK)return result;
    result=ksn_notice_emit(system,tx,notice,state->notice_resource,variant);
    if(result==KSN_OK)result=ksn_view_submit(system,tx);
    if(result!=KSN_OK){ksn_view_cancel(system,tx);return result;}
    state->notice_tx=tx;state->notice_pending=id;state->notice_pending_variant=variant;
    return KSN_OK;
}
bool pocket_kasane_notice_composited(void){
    return !dormant&&state&&ksn_runtime_app_system_view(state->lease)&&
        (state->notice_displayed||(state->notice_tx.value&&state->notice_pending));
}
bool pocket_kasane_system_pending(void){return !dormant&&state&&ksn_runtime_app_system_view(state->lease)&&state->notice_tx.value;}
bool pocket_kasane_has_submission(void) {
    return ksn_runtime_has_submission();
}
uint32_t pocket_kasane_source_wait_ticks(uint64_t now_us,uint32_t cap,uint32_t hz){
    if(dormant||!state||!state->schema||!hz)return cap;
    schema_sources *native=schema_native(state->schema);
    schema_externals *external=schema_external_state(state->schema);
    uint64_t next=native&&native->expiry_us?native->expiry_us:UINT64_MAX;
    if(external&&external->expiry_us&&external->expiry_us<next)
        next=external->expiry_us;
    /* A due deadline is retried by the regular owner frame. Returning zero
     * here could busy-spin after a failed or pending submission. */
    if(next==UINT64_MAX||next<=now_us)return cap;
    uint64_t delta=next-now_us,seconds=delta/1000000u;
    if(seconds>cap/hz)return cap;
    uint64_t ticks=seconds*hz+((delta%1000000u)*hz+999999u)/1000000u;
    return ticks<cap?(uint32_t)ticks:cap;
}
ksn_result pocket_kasane_advance(uint64_t now_us){
    apply_outcome();return ksn_runtime_advance_animations(now_us);
}
bool pocket_kasane_animation_pending(void){return ksn_runtime_animation_pending();}
unsigned pocket_kasane_defer_block(void){
    if(dormant||!state||!state->active)return 1u;
    unsigned why=0;
    if(state->schema||state->provider)why|=2u;
    if(state->video_resource.value||state->pixel_resource.value)why|=4u;
    for(unsigned i=0;i<POCKET_GRID_MAX_SLOTS;i++)if(state->grid_resources[i].value)why|=4u;
    if(state->notice_tx.value||state->notice_displayed||state->notice_pending)why|=8u;
    if(ksn_runtime_animation_enabled())why|=16u;
    /* The modal itself, not the input scope: the scope also reads BLOCKED
     * whenever bands are merely invalidated, which every Shiki commit does. */
    if(ksn_runtime_modal_active())why|=32u;
    return why;
}
bool pocket_kasane_present_deferrable(void){return pocket_kasane_defer_block()==0;}
void pocket_kasane_animations_presented(uint64_t now_us){ksn_runtime_animations_presented(now_us);}
void pocket_kasane_set_animation_time(uint64_t now_us){
    owner_now_us=now_us;
    ksn_runtime_set_animation_time(now_us);
}
bool pocket_kasane_needs_present(void) {
    return ksn_runtime_needs_present();
}
void pocket_kasane_invalidate_bands(uint32_t bands) {
    ksn_runtime_invalidate_bands(bands);
}
uint32_t pocket_kasane_opaque_system_bands(void){
    return !dormant&&state&&state->active?ksn_runtime_opaque_system_bands():0;
}
void pocket_kasane_invalidate(void) {
    ksn_runtime_invalidate();
}
#ifdef KASANE_DMA_OVERLAP
ksn_result pocket_kasane_present(const ksn_display_port *display,ksn_render_stats *stats) {
    return pocket_kasane_present_complete(display,NULL,stats);
}
void pocket_kasane_present_begin(pocket_kasane_present_job *job) {
    job->after=false;job->present.running=false;
    ksn_render_stats *stats=job->present.render.stats;
    const ksn_display_completion *completion=job->present.render.completion;
    if(!stats) {job->result=KSN_INVALID;return;}
    *stats=(ksn_render_stats){0};
    /* No renderer will run on the empty path below. Complete any earlier
     * board work before its no-damage native acknowledgements as well. */
    if(!pocket_kasane_needs_present()&&completion&&completion->finish){
        ksn_result done=completion->finish(completion->ctx);
        if(done!=KSN_OK) {job->result=done;return;}
    }
    if(pocket_proc_pending()&&!pocket_kasane_needs_present()){
        if(pocket_proc_backdrop_pending())ksn_runtime_invalidate();
        else pocket_proc_present_result(KSN_OK);
    }
    if(pocket_grid_pending()&&!pocket_kasane_needs_present())
        pocket_grid_present_result(KSN_OK);
    if(pocket_video_pending()&&!pocket_kasane_needs_present())
        pocket_video_present_result(KSN_OK);
    if(pocket_pixel_pending()&&!pocket_kasane_needs_present())
        pocket_pixel_present_result(KSN_OK);
    if(!pocket_kasane_needs_present()) {job->result=KSN_OK;return;}
    job->proc_candidate=pocket_proc_pending();
    if(job->present.render.display)pocket_proc_prepare_render(job->present.render.display->strip_rows);
    job->present.render.load=pocket_proc_has_frame()&&!pocket_proc_is_image_mode()?
        pocket_proc_backdrop:NULL;
    job->present.render.occlusion_safe=false;
    job->after=true;
    job->result=ksn_runtime_present_begin(&job->present);
}
ksn_result pocket_kasane_present_end(pocket_kasane_present_job *job) {
    if(!job->after)return job->result;
    job->after=false;
    ksn_render_stats *stats=job->present.render.stats;
    ksn_result result=ksn_runtime_present_end(&job->present);
    if(result==KSN_OK&&pocket_proc_backdrop_pending()&&!stats->transferred_bytes)
        ksn_runtime_invalidate();
    else pocket_proc_present_result(result);
    pocket_grid_present_result(result);
    pocket_video_present_result(result);
    pocket_pixel_present_result(result);
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    if(job->proc_candidate&&result==KSN_OK&&stats->transferred_bytes){
        ESP_LOGI("PROC_JS","frame=%lu bytes=%lu",
                 (unsigned long)proc_log_frame++,(unsigned long)stats->transferred_bytes);
    }
#endif
    apply_outcome();return result;
}
ksn_result pocket_kasane_present_complete(const ksn_display_port *display,
                                          const ksn_display_completion *completion,
                                          ksn_render_stats *stats) {
    pocket_kasane_present_job job={.present={.render={.display=display,.completion=completion,
                                                      .stats=stats}}};
    pocket_kasane_present_begin(&job);
    ksn_present_job_run(&job.present);
    return pocket_kasane_present_end(&job);
}
#else
ksn_result pocket_kasane_present(const ksn_display_port *display,ksn_render_stats *stats) {
    if(!stats) return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    if(pocket_proc_pending()&&!pocket_kasane_needs_present()){
        if(pocket_proc_backdrop_pending())ksn_runtime_invalidate();
        else pocket_proc_present_result(KSN_OK);
    }
    if(pocket_grid_pending()&&!pocket_kasane_needs_present())
        pocket_grid_present_result(KSN_OK);
    if(pocket_video_pending()&&!pocket_kasane_needs_present())
        pocket_video_present_result(KSN_OK);
    if(pocket_pixel_pending()&&!pocket_kasane_needs_present())
        pocket_pixel_present_result(KSN_OK);
    if(!pocket_kasane_needs_present()) return KSN_OK;
    bool proc_candidate=pocket_proc_pending();
    if(display)pocket_proc_prepare_render(display->strip_rows);
    ksn_result result=pocket_proc_has_frame()&&!pocket_proc_is_image_mode()?
        ksn_runtime_present_backdrop(display,pocket_proc_backdrop,false,stats):
        ksn_runtime_present(display,stats);
    if(result==KSN_OK&&pocket_proc_backdrop_pending()&&!stats->transferred_bytes)
        ksn_runtime_invalidate();
    else pocket_proc_present_result(result);
    pocket_grid_present_result(result);
    pocket_video_present_result(result);
    pocket_pixel_present_result(result);
#ifdef KASANE_PROC_JS_DIAGNOSTIC
    if(proc_candidate&&result==KSN_OK&&stats->transferred_bytes){
        ESP_LOGI("PROC_JS","frame=%lu bytes=%lu",
                 (unsigned long)proc_log_frame++,(unsigned long)stats->transferred_bytes);
    }
#else
    (void)proc_candidate;
#endif
    apply_outcome();return result;
}
#endif
ksn_result pocket_kasane_present_backdrop(const ksn_display_port *display,
                                          ksn_backdrop_loader load,bool occlusion_safe,
                                          ksn_render_stats *stats) {
    if(!stats||!load)return KSN_INVALID;
    *stats=(ksn_render_stats){0};
    if(!pocket_kasane_needs_present()){
        pocket_proc_overlay_present_result(KSN_OK);
        pocket_grid_present_result(KSN_OK);
        pocket_video_present_result(KSN_OK);
        pocket_pixel_present_result(KSN_OK);
        return KSN_OK;
    }
    if(display)pocket_proc_prepare_render(display->strip_rows);
    ksn_result result=ksn_runtime_present_backdrop(display,load,occlusion_safe,stats);
    pocket_proc_overlay_present_result(result);
    pocket_grid_present_result(result);
    pocket_video_present_result(result);
    pocket_pixel_present_result(result);
    apply_outcome();return result;
}
void pocket_kasane_end_turn(void) {
    pocket_proc_end_turn();
    pocket_kasane_park_turn();
}
/* The view builder is aborted either way: run_build() opens and submits it
 * inside one native call, and a native re-entry is never a floor the VM may
 * park on (vm-L2-design sec.11.2), so a builder still open here is a failed
 * build. Only the procedural frame spans several native calls, and only it
 * has to survive a park. */
void pocket_kasane_park_turn(void) {
    if(state) { ksn_runtime_app_end_turn(state->lease); apply_outcome(); }
}
ksn_input_scope pocket_kasane_input_scope(bool host_priority) {
    ksn_input_scope scope=ksn_runtime_input_scope(host_priority);
    /* P2a (two-core-pipeline.md sec.7.3): a frame on the display task keeps
     * its repair bands until it is acknowledged, so the scope reads BLOCKED
     * for the whole time it is in flight. Without the overlap the next turn
     * starts after that present, so it is answered as it would be then. A
     * modal or a pending modal change still blocks; a frame that then fails
     * to reach the panel ends deferring for the session. */
    if(scope==KSN_INPUT_BLOCKED&&disp_busy()&&!ksn_runtime_modal_active())
        scope=KSN_INPUT_APP;
    return scope;
}
