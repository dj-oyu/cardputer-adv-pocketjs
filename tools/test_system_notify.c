#include <assert.h>
#include <stdio.h>
#include "../main/system/sys_notify.c"
int main(void){
    sys_notify s={0};sys_notice active;uint32_t ids[9],duplicate;
    sys_notify_step(&s,0);assert(sys_notify_deadline(&s)==UINT64_MAX&&!sys_notify_take_changed(&s));
    assert(sys_notify_post(&s,1,1,"FIRST",0,&ids[0])==NOTICE_OK);
    assert(sys_notify_post(&s,1,1,"IGNORED",99,&duplicate)==NOTICE_OK&&duplicate==ids[0]);
    sys_notify_step(&s,0);assert(sys_notify_active(&s,&active)&&active.id==ids[0]&&!strcmp(active.label,"FIRST"));
    assert(sys_notify_take_changed(&s));
    uint32_t revision=s.revision;
    for(uint64_t now=0;now<60000000;now+=1000)sys_notify_step(&s,now);
    assert(s.revision==revision&&!sys_notify_take_changed(&s)&&sys_notify_deadline(&s)==UINT64_MAX);
    for(int i=1;i<9;i++)assert(sys_notify_post(&s,2,0,"WAIT",0,&ids[i])==NOTICE_OK);
    assert(sys_notify_post(&s,2,0,"FULL",0,&duplicate)==NOTICE_FULL);
    assert(sys_notify_snooze(&s,1,ids[0],100)==NOTICE_FULL);
    assert(sys_notify_active(&s,&active)&&active.id==ids[0]);
    assert(sys_notify_ack(&s,2,ids[0])==NOTICE_GONE);
    assert(sys_notify_cancel(&s,2,ids[1])==NOTICE_OK);
    assert(sys_notify_snooze(&s,1,ids[0],100)==NOTICE_OK);
    sys_notify_step(&s,50);assert(sys_notify_active(&s,&active)&&active.id==ids[2]);
    s.next_order=UINT64_MAX;sys_notify_step(&s,100);
    for(int i=2;i<9;i++){
        assert(sys_notify_active(&s,&active)&&active.id==ids[i]);
        assert(sys_notify_ack(&s,2,ids[i])==NOTICE_OK);sys_notify_step(&s,100);
    }
    assert(sys_notify_active(&s,&active)&&active.id==ids[0]);
    assert(sys_notify_ack(&s,1,ids[0])==NOTICE_OK&&sys_notify_ack(&s,1,ids[0])==NOTICE_GONE);
    assert(sys_notify_post(&s,1,0,"TTL",200,&ids[0])==NOTICE_OK);
    sys_notify_step(&s,100);assert(sys_notify_active(&s,&active));
    sys_notify_step(&s,200);assert(!sys_notify_active(&s,&active));
    assert(sys_notify_cancel(&s,1,ids[0])==NOTICE_GONE);
    assert(sys_notify_post(&s,1,0,"SYSTEM",0,&ids[0])==NOTICE_OK);
    for(int cycle=0;cycle<100;cycle++){
        assert(sys_notify_post(&s,2,0,"APP",0,&ids[1])==NOTICE_OK);
        sys_notify_release_owner(&s,2);
    }
    sys_notify_step(&s,200);assert(sys_notify_active(&s,&active)&&active.id==ids[0]);
    sys_notify_release_owner(&s,1);assert(!sys_notify_active(&s,&active));
    assert(sys_notify_post(&s,1,0,"",0,&duplicate)==NOTICE_INVALID);
    assert(sys_notify_post(&s,1,0,"1234567890123456789012345",0,&duplicate)==NOTICE_INVALID);
    last_notice=UINT32_MAX-1;
    assert(sys_notify_post(&s,1,0,"LAST",0,&duplicate)==NOTICE_OK&&duplicate==UINT32_MAX);
    sys_notify_release_owner(&s,1);
    assert(sys_notify_post(&s,1,0,"EXHAUSTED",0,&duplicate)==NOTICE_FULL);
    printf("SYSTEM_NOTIFY_OK FIFO, 8+1, snooze pressure, TTL, dedupe, owner lifetime, exhaustion record=%zu store=%zu\n",sizeof(sys_notice),sizeof(s));
}
