/* Reuse the real QuickJS/native owner fixture. Input delivery itself has the
 * separate input-service suite; here the app receives deterministic actions. */
#define main kasane_full_contract_main
#include "test_pocket_kasane.c"
#undef main
static void hello_count(ksn_view *system,const char *expected){
    ksn_frame frame;ksn_frame_command cmd;
    check(ksn_core_frame(system->host->core,&frame)==KSN_OK&&
          ksn_core_read(system->host->core,frame.ticket,false,KSN_APP,3,&cmd)==KSN_OK&&
          cmd.draw.kind==KSN_TEXT&&cmd.draw.data.text.bytes==strlen(expected)&&
          !memcmp(cmd.text,expected,strlen(expected)),"hello snapshot matches domain input count");
}
int main(void){
    check(open_fault_runtime("globalThis.pocket={kasane:kasane,input:{onAction(fn){globalThis.action=fn}}};"
          "globalThis.console={log(){}};"),"hello runtime opens without legacy UI");
    FILE *file=fopen("apps/hello/main.js","rb");if(!file)return 2;
    char source[8192];size_t bytes=fread(source,1,sizeof(source)-1,file);fclose(file);source[bytes]=0;
    ksn_view *system=NULL;check(ksn_runtime_system_acquire(&system)==KSN_OK,"hello system peer opens");
    ksn_tx blocker;check(ksn_view_begin(system,KSN_REPLACE,&blocker)==KSN_OK,"SYSTEM holds builder");
    check(run(source)&&!pocket_kasane_has_submission(),"hello initial BUSY is retryable");
    check(run("action({action:'accept',phase:'press'});action({action:'accept',phase:'release'});"
              "action({action:'accept',phase:'press'});frame();"),"input progresses while display is BUSY");
    check(ksn_view_cancel(system,blocker)==KSN_OK&&run("frame()"),"hello retries when SYSTEM releases builder");
    hello_count(system,"KEY PRESSES: 2");
    ksn_render_stats stats;fail_once=true;
    check(present(&stats)==KSN_IO,"hello first REPLACE partially fails");
    check(run("action({action:'accept',phase:'press'});frame();"),"input progresses while presentation retries");
    hello_count(system,"KEY PRESSES: 2");
    check(run("testCancel();frame();"),"cancelled candidate refs are rebuilt");
    hello_count(system,"KEY PRESSES: 3");
    check(present(&stats)==KSN_OK&&run("frame()"),"hello replacement presents and promotes refs");
    transfers=0;check(run("frame();frame();frame()")&&present(&stats)==KSN_OK&&transfers==0,
          "idle hello submits and transfers nothing");
    check(run("action({action:'accept',phase:'repeat'});action({action:'right',phase:'press'});"
              "action({action:'accept',phase:'press'});frame();"),"only accept press changes count");
    hello_count(system,"KEY PRESSES: 4");
    check(present(&stats)==KSN_OK&&stats.bands==((1u<<9)|(1u<<10)|(1u<<11)),"hello PATCH sends only counter bands");
    close_fault_runtime();check(ksn_runtime_shutdown()==KSN_OK&&live_allocations==0,"hello teardown frees memory");
    printf("HELLO_APP %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
