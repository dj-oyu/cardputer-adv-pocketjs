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
    check(run("kasane.cancel(kasane.poll().ticket);frame();"),"cancelled candidate refs are rebuilt");
    hello_count(system,"KEY PRESSES: 3");
    check(present(&stats)==KSN_OK&&run("frame()"),"hello replacement presents and promotes refs");
    transfers=0;check(run("frame();frame();frame()")&&present(&stats)==KSN_OK&&transfers==0,
          "idle hello submits and transfers nothing");
    check(run("action({action:'accept',phase:'repeat'});action({action:'right',phase:'press'});"
              "action({action:'accept',phase:'press'});frame();"),"only accept press changes count");
    hello_count(system,"KEY PRESSES: 4");
    check(present(&stats)==KSN_OK&&stats.bands==((1u<<9)|(1u<<10)|(1u<<11)),"hello PATCH sends only counter bands");
    check(run("frame();globalThis.scene=kasane.createScene({build(tx,state){tx.background(255);"
              "return {label:tx.text({bounds:[0,0,120,16],text:String(state),capacity:8,color:0xffffffff})}},"
              "patch(tx,refs,state){refs.label.setText(tx,String(state))}});scene.flush(1);"),
          "controller supports reusable explicit topology rebuild");
    check(present(&stats)==KSN_OK&&run("scene.flush(1);scene.invalidate(true);scene.flush(2);"
              "kasane.cancel(kasane.poll().ticket);scene.flush(3);"),"failed rebuild retries with latest state");
    check(present(&stats)==KSN_OK&&run("scene.flush(3);scene.invalidate();scene.flush(4);"),
          "subsequent PATCH uses promoted refs rather than cancelled refs");
    check(present(&stats)==KSN_OK,"controller recovery patch presents");
    check(run("let s=kasane.createScene({build(tx){tx.background(255);return Promise.resolve({})}});"
              "let caught=false;try{s.flush({})}catch(e){caught=e.code==='INVALID_ARGUMENT'}"
              "if(!caught)throw Error('async build accepted')"),"controller does not hide asynchronous build results");
    check(!pocket_kasane_has_submission(),"async controller leaves no partial submission");
    /* The controller is native (pocket_kasane.c); these pin the statements of
     * apps/kasane/create_scene.js it transcribes. */
    check(run("for(const bad of [undefined,null,0,1,{},{build:1},{build(){},patch:1},{build(){},patch:null}]){"
              "let t=false;try{kasane.createScene(bad)}catch(e){t=e instanceof TypeError}"
              "if(!t)throw Error('accepted '+JSON.stringify(bad))}"
              "let got=0;const lazy={get build(){got++;return 1},get patch(){got+=10}};"
              "try{kasane.createScene(lazy)}catch(e){}if(got!==1)throw Error('patch read after bad build');"
              "const plain=kasane.createScene({build(tx){tx.background(255);return {}}});"
              "if(Object.keys(plain).join()!=='invalidate,flush'||plain.flush.length!==1||"
              "plain.invalidate.length!==1||plain.flush.name!=='flush')throw Error('shape');"),
          "createScene validates options and returns the object-literal shape");
    check(present(&stats)==KSN_OK&&run(
              "let n=0;const s2=kasane.createScene({build(tx,m){n++;tx.background(255);"
              "if(m==='fn')return function(){};if(m==='null')return null;return {}}});"
              "for(const m of ['fn','null']){let t=false;try{s2.flush(m)}catch(e){t=e instanceof TypeError}"
              "if(!t||kasane.poll().status==='SUBMITTED')throw Error('bad build return '+m)}"
              "if(s2.flush('ok')!==false||n!==3)throw Error('dirty kept after error');"),
          "a build returning a function or null throws TypeError and stays dirty");
    check(present(&stats)==KSN_OK&&run(
              "let inner;const s3=kasane.createScene({build(tx){tx.background(255);"
              "try{s3.flush()}catch(e){inner=e}return {}}});const f=s3.flush;"
              "if(f()!==false||!(inner instanceof Error)||inner.message!=='scene flush is not reentrant')"
              "throw Error('reentry');"),
          "flush rejects reentry and works when detached");
    check(present(&stats)==KSN_OK&&run(
              "const marker={code:'LIMIT_EXCEEDED'};let calls=0;"
              "const s4=kasane.createScene({build(tx){calls++;tx.background(255);return {}},"
              "patch(tx){calls++;if(calls===2)throw marker}});"
              "s4.flush();globalThis.s4=s4;globalThis.marker=marker;"),"patch fixture presents");
    check(present(&stats)==KSN_OK&&run(
              "if(s4.flush()!==true)throw Error('idle');s4.invalidate();"
              "let t;try{s4.flush()}catch(e){t=e}if(t!==marker)throw Error('rethrow identity');"
              "if(s4.flush()!==false||kasane.poll().status!=='SUBMITTED')throw Error('retry after throw');"
              "kasane.cancel(kasane.poll().ticket);if(s4.flush()!==false)throw Error('discard retry');"
              "kasane.cancel(kasane.poll().ticket);"
              "const busy=kasane.createScene({build(){throw {code:'BUSY'}}});"
              "if(busy.flush()!==false)throw Error('BUSY swallowed');"
              "const bad=kasane.createScene({build(){throw 0}});let z=1;try{bad.flush()}catch(e){z=e}"
              "if(z!==0)throw Error('falsy rethrown');"),
          "non-BUSY errors propagate by identity; BUSY and DISCARDED retry");
    check(present(&stats)==KSN_OK,"controller error fixture presents");
    close_fault_runtime();check(ksn_runtime_shutdown()==KSN_OK&&live_allocations==0,"hello/controller teardown frees memory");
    fault_sweep("scene controller allocation failures publish nothing",
                "globalThis.exercise=()=>{const s=kasane.createScene({build(tx,m){tx.background(255);"
                "return {r:tx.rect(shape),m}},patch(tx,r){r.r.setColor(tx,1)}});s.flush(1)};",false);
    /* A build that captures its own scene forms a cycle through the holder;
     * only gc_mark lets the collector free it. */
    check(open_fault_runtime("for(let i=0;i<20;i++){const s=kasane.createScene({build(tx){"
              "s.invalidate();tx.background(255);return {s}}});s.flush(s);}"),"cyclic scenes build");
    close_fault_runtime();check(live_allocations==0,"cyclic scene graphs are collected");
    printf("HELLO_APP %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
