#include <assert.h>
#include <stdio.h>
#include "../main/system/sys_notify.c"
#include "../main/system/sys_timer.c"
int main(void){
    sys_notify n={0};sys_timer t={0};uint32_t ids[9];sys_notice active;
    sys_timer_step(&t,&n,0,false);assert(sys_timer_deadline(&t)==UINT64_MAX);
    assert(sys_timer_set(&t,1,"a","ONE",100)==NOTICE_OK);
    assert(sys_timer_set(&t,2,"a","TWO",200)==NOTICE_OK);
    assert(sys_timer_set(&t,1,"b","THREE",300)==NOTICE_OK);
    assert(sys_timer_set(&t,1,"c","FOUR",400)==NOTICE_OK);
    assert(sys_timer_set(&t,1,"d","FULL",500)==NOTICE_FULL);
    assert(sys_timer_set(&t,1,"missing","CANCEL",0)==NOTICE_OK);
    assert(sys_timer_deadline(&t)==100);
    sys_timer_record snapshot;
    assert(sys_timer_read(&t,1,"a",&snapshot)&&snapshot.due_us==100);
    assert(sys_timer_read(&t,2,"a",&snapshot)&&snapshot.due_us==200);
    assert(!sys_timer_read(&t,3,"a",&snapshot));
    assert(sys_timer_take_changed(&t));
    assert(sys_timer_set(&t,1,"a","ONE",100)==NOTICE_OK&&!sys_timer_take_changed(&t));
    sys_timer_step(&t,&n,99,false);assert(!sys_notify_take_changed(&n));
    sys_timer_step(&t,&n,100,false);sys_notify_step(&n,100);
    assert(sys_notify_active(&n,&active)&&!strcmp(active.label,"ONE"));
    assert(sys_timer_deadline(&t)==200);
    sys_timer_release_owner(&t,1);assert(sys_timer_deadline(&t)==200);
    sys_timer_step(&t,&n,200,false);assert(sys_timer_deadline(&t)==UINT64_MAX);
    sys_notify_release_owner(&n,1);sys_notify_release_owner(&n,2);
    assert(sys_notify_post(&n,1,0,"ACTIVE",0,&ids[0])==NOTICE_OK);sys_notify_step(&n,200);
    for(int i=1;i<9;i++)assert(sys_notify_post(&n,1,0,"WAIT",0,&ids[i])==NOTICE_OK);
    assert(sys_timer_set(&t,2,"blocked","RETRY",300)==NOTICE_OK);
    sys_timer_step(&t,&n,300,false);assert(sys_timer_deadline(&t)==UINT64_MAX);
    assert(t.records[0].due_us==300&&t.records[0].blocked);
    sys_timer_take_changed(&t);sys_timer before=t;uint32_t sequence=last_notice;
    for(uint64_t now=300;now<60000000;now+=1000)sys_timer_step(&t,&n,now,false);
    assert(!memcmp(&before,&t,sizeof(t))&&last_notice==sequence);
    assert(sys_notify_cancel(&n,1,ids[1])==NOTICE_OK);
    sys_timer_step(&t,&n,60000000,true);assert(!t.records[0].due_us);
    assert(sys_timer_take_changed(&t));
    sys_notify_release_owner(&n,1);sys_notify_step(&n,60000000);
    assert(sys_notify_active(&n,&active)&&active.owner==2&&!strcmp(active.label,"RETRY"));
    assert(sys_timer_set(&t,1,"invalid","NO",UINT64_MAX)==NOTICE_INVALID);
    printf("SYSTEM_TIMER_OK four slots, owner isolation, deadlines, idempotent cancel, retained full-queue due, no retry spin record=%zu store=%zu\n",sizeof(sys_timer_record),sizeof(t));
}
