/* Actual app/renderer teardown regression, not the 24-surface registry test. */
#define CHARACTER_HOST_NO_MAIN
#include "test_character_app_host.c"

int main(void){
    const char *order[]={"A","C","A","C"};unsigned completed=0,frame_calls=0;
    const char *clear[]={"CHARACTER_MEMORY_PIXELS","CHARACTER_CAPTURE_DIR","CHARACTER_CAPTURE_PLAN",
        "CHARACTER_MODEL_PIXELS","CHARACTER_MEMORY_STUDY","CHARACTER_SOURCE_PLANE","CHARACTER_FIXTURE","MEGA_PPM"};
    for(unsigned i=0;i<sizeof clear/sizeof clear[0];i++)unsetenv(clear[i]);
    if(setenv("CHARACTER_APP_ENTRY","apps/cloudmem/cloud_memory.js",1)||
       setenv("CHARACTER_APP_CHUNKS","apps/cloudmem/chunks.txt",1)||
       setenv("CHARACTER_MEMORY_PROFILE","DEVICE_SHORT",1)||
       setenv("CHARACTER_NO_EAGER_GC","1",1)||setenv("CHARACTER_FULL_REFERENCE","1",1))return 2;
    if(sizeof(void*)==4){if(setenv("CHARACTER_HEAP_LIMIT","163840",1))return 2;}else unsetenv("CHARACTER_HEAP_LIMIT");
    char *args[]={"test-class-session-order",NULL};
    for(unsigned i=0;i<4;i++){
        if(cur_bytes){fprintf(stderr,"CLASS_SESSION_ORDER retained_before_session=%zu\n",cur_bytes);return 1;}
        chunks=0;memset(chunk,0,sizeof chunk);strip_debug=false;memory_frames=0;peak_bytes=0;largest_guest_request=0;
        if(setenv("CHARACTER_MEMORY_APP",order[i],1))return 2;
        printf("CLASS_SESSION_START index=%u variant=%s pointer_bits=%zu cap=%u\n",i,order[i],sizeof(void*)*8,sizeof(void*)==4?163840u:0u);fflush(stdout);
        int result=character_host_run(1,args);frame_calls+=memory_frames;
        printf("CLASS_SESSION_END index=%u variant=%s result=%d frame_calls=%u released_guest=%zu exceptions=%u present_failures=%u\n",
            i,order[i],result,memory_frames,cur_bytes,exceptions,present_failures);fflush(stdout);
        if(result||cur_bytes||exceptions||present_failures)return 1;
        ++completed;
    }
    /* Each inherited contract validates two complete 63-draw runs. UI and
     * control turns also present/full-reference, so report them separately. */
    printf("CLASS_SESSION_ORDER_PASS sessions=%u successful_draws=%u frame_calls=%u released_guest=%zu full_reference=1 eager_gc=0 pointer_bits=%zu cap=%u\n",
        completed,completed*2*63,frame_calls,cur_bytes,sizeof(void*)*8,sizeof(void*)==4?163840u:0u);
    return completed==4?0:1;
}
