/* Host only, synthetic command/I/O counters; no firmware or device dependency.
 * cc -std=c11 -O2 -Wall -Wextra -Werror -pthread tools/test_diagnostic_command.c -o /tmp/test-diagnostic-command
 * /tmp/test-diagnostic-command
 * ASan/UBSan: replace -O2 with -O1 -g -fsanitize=address,undefined.
 * TSan: replace -O2 with -O1 -g -fsanitize=thread (run separately).
 * A sanitizer startup failure is not a passing race check.
 */
#define _POSIX_C_SOURCE 200809L
#include "../main/ui/diagnostic_command.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    diagnostic_command command;
    pthread_barrier_t barrier;
    pthread_t ui;
    unsigned mode,seed;
    atomic_bool done;
    unsigned io,suppressed,producer_calls;
} fixture;
static void barrier(fixture*f){int r=pthread_barrier_wait(&f->barrier);assert(r==0||r==PTHREAD_BARRIER_SERIAL_THREAD);}
static void io(fixture*f){assert(pthread_equal(pthread_self(),f->ui));f->io++;}
static void complete(fixture*f){diagnostic_finish_result r=diagnostic_command_finish(&f->command,true);assert(r==DIAG_FINISH_COMPLETE||r==DIAG_FINISH_CANCELLED);}
static void*forced_producer(void*p){
    fixture*f=p;diagnostic_command*c=&f->command;barrier(f);
    assert(diagnostic_command_request(c)==DIAG_REQUEST_QUEUED);
    assert(diagnostic_command_request(c)==DIAG_REQUEST_COALESCED);
    if(f->mode==0)assert(diagnostic_command_cancel(c)==DIAG_CANCEL_SUPPRESSED);
    barrier(f);barrier(f);
    if(f->mode==1){assert(diagnostic_command_cancel(c)==DIAG_CANCEL_DEFERRED);assert(diagnostic_command_request(c)==DIAG_REQUEST_BUSY);}
    if(f->mode==2)assert(diagnostic_command_cancel(c)==DIAG_CANCEL_DEFERRED);
    if(f->mode==3){diagnostic_cancel_result r=diagnostic_command_cancel(c);assert(r==DIAG_CANCEL_IDLE||r==DIAG_CANCEL_DEFERRED);}
    if(f->mode==5){diagnostic_cancel_result r=diagnostic_command_cancel(c);assert(r==DIAG_CANCEL_DEFERRED||r==DIAG_CANCEL_TERMINAL);}
    barrier(f);
    if(f->mode==4)assert(diagnostic_command_request(c)==DIAG_REQUEST_QUEUED);
    if(f->mode==5)for(unsigned i=0;i<100;i++){
        assert(diagnostic_command_request(c)==DIAG_REQUEST_TERMINAL);
        assert(diagnostic_command_cancel(c)==DIAG_CANCEL_TERMINAL);
    }
    barrier(f);barrier(f);return NULL;
}
static void*forced_ui(void*p){
    fixture*f=p;diagnostic_command*c=&f->command;f->ui=pthread_self();barrier(f);barrier(f);
    if(f->mode==0)assert(!diagnostic_command_take(c));else assert(diagnostic_command_take(c));
    if(f->mode>=3){assert(diagnostic_command_begin(c)==DIAG_BEGIN_RUN);io(f);}
    barrier(f);diagnostic_begin_result began=DIAG_BEGIN_NOT_CLAIMED;
    if(f->mode==2){began=diagnostic_command_begin(c);assert(began==DIAG_BEGIN_RUN||began==DIAG_BEGIN_CANCELLED);if(began==DIAG_BEGIN_RUN)io(f);else f->suppressed++;}
    if(f->mode==3||f->mode==4)complete(f);
    if(f->mode==5)assert(diagnostic_command_finish(c,false)==DIAG_FINISH_TERMINAL);
    barrier(f);
    if(f->mode==1){assert(diagnostic_command_begin(c)==DIAG_BEGIN_CANCELLED);f->suppressed++;}
    if(f->mode==2&&began==DIAG_BEGIN_RUN)assert(diagnostic_command_finish(c,true)==DIAG_FINISH_CANCELLED);
    barrier(f);
    if(f->mode==4){assert(diagnostic_command_take(c));assert(diagnostic_command_begin(c)==DIAG_BEGIN_RUN);io(f);complete(f);}
    assert(atomic_load(&c->state)==(f->mode==5?DIAG_CMD_TERMINAL:DIAG_CMD_IDLE));
    barrier(f);return NULL;
}
static uint32_t random_next(uint32_t*s){uint32_t x=*s;x^=x<<13;x^=x>>17;x^=x<<5;return *s=x;}
static void*random_producer(void*p){fixture*f=p;uint32_t s=f->seed;barrier(f);
    for(unsigned i=0;i<100000;i++){
        if(random_next(&s)&1){diagnostic_request_result r=diagnostic_command_request(&f->command);assert(r<=DIAG_REQUEST_TERMINAL);}
        else{diagnostic_cancel_result r=diagnostic_command_cancel(&f->command);assert(r<=DIAG_CANCEL_TERMINAL);}
        f->producer_calls++;assert(atomic_load(&f->command.state)<=DIAG_CMD_TERMINAL);
        if((random_next(&s)&15)==0)sched_yield();
    }
    atomic_store(&f->done,true);return NULL;
}
static void*random_ui(void*p){fixture*f=p;f->ui=pthread_self();barrier(f);
    for(;;){
        if(diagnostic_command_take(&f->command)){
            sched_yield();diagnostic_begin_result r=diagnostic_command_begin(&f->command);
            if(r==DIAG_BEGIN_RUN){io(f);sched_yield();
                if((f->seed&1)&&f->io==64)assert(diagnostic_command_finish(&f->command,false)==DIAG_FINISH_TERMINAL);
                else complete(f);
            }else{assert(r==DIAG_BEGIN_CANCELLED);f->suppressed++;}
        }else{
            if(atomic_load(&f->command.state)==DIAG_CMD_TERMINAL)break;
            /* Reload after done: a previously sampled IDLE could precede the
             * producer's final accepted request. */
            if(atomic_load(&f->done)&&atomic_load(&f->command.state)==DIAG_CMD_IDLE)break;
            sched_yield();
        }
    }return NULL;
}
static void run_pair(fixture*f,void*(*producer)(void*),void*(*consumer)(void*)){
    pthread_t a,b;assert(pthread_barrier_init(&f->barrier,NULL,2)==0);
    assert(pthread_create(&a,NULL,producer,f)==0);assert(pthread_create(&b,NULL,consumer,f)==0);
    assert(pthread_join(a,NULL)==0);assert(pthread_join(b,NULL)==0);assert(pthread_barrier_destroy(&f->barrier)==0);
}
int main(void){
    diagnostic_command a=DIAGNOSTIC_COMMAND_INIT,b=DIAGNOSTIC_COMMAND_INIT;
    assert(diagnostic_command_begin(&a)==DIAG_BEGIN_NOT_CLAIMED);
    assert(diagnostic_command_finish(&a,true)==DIAG_FINISH_NOT_RUNNING);
    assert(diagnostic_command_request(&a)==DIAG_REQUEST_QUEUED);
    assert(diagnostic_command_finish(&a,false)==DIAG_FINISH_NOT_RUNNING);
    assert(atomic_load(&a.state)==DIAG_CMD_PENDING&&atomic_load(&b.state)==DIAG_CMD_IDLE);
    assert(diagnostic_command_take(&a));assert(diagnostic_command_begin(&a)==DIAG_BEGIN_RUN);
    assert(diagnostic_command_finish(&a,false)==DIAG_FINISH_TERMINAL);
    assert(diagnostic_command_finish(&a,true)==DIAG_FINISH_TERMINAL);
    assert(diagnostic_command_begin(&a)==DIAG_BEGIN_NOT_CLAIMED);
    assert(!diagnostic_command_take(&a));assert(diagnostic_command_request(&a)==DIAG_REQUEST_TERMINAL);
    assert(diagnostic_command_cancel(&a)==DIAG_CANCEL_TERMINAL);
    for(unsigned mode=0;mode<6;mode++)for(unsigned n=0;n<64;n++){
        /* Fresh instance per scenario. No reset/recovery of live state. */
        fixture f={.command=DIAGNOSTIC_COMMAND_INIT,.mode=mode};run_pair(&f,forced_producer,forced_ui);
        if(mode<=1)assert(f.io==0);
        if(mode==4)assert(f.io==2);
    }
    for(unsigned seed=1;seed<=8;seed++){
        fixture f={.command=DIAGNOSTIC_COMMAND_INIT,.seed=seed};run_pair(&f,random_producer,random_ui);
        assert(f.producer_calls==100000);unsigned state=atomic_load(&f.command.state);
        assert(state==DIAG_CMD_IDLE||state==DIAG_CMD_TERMINAL);
    }
    puts("PASS diagnostic command: independent instances, 384 forced rounds, 800000 randomized producer operations");
}
