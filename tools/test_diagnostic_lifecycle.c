/* Host only, synthetic tokens. From repository root:
 * cc -std=c11 -O2 -Wall -Wextra -Werror tools/test_diagnostic_lifecycle.c -o /tmp/test-diagnostic-life
 * /tmp/test-diagnostic-life
 * Sanitizers: replace -O2 with -O1 -g -fsanitize=address,undefined.
 */
#include "../main/ui/diagnostic_lifecycle.h"
#include <assert.h>
#include <stdio.h>
typedef struct {int fail,acquires,drains,works,restores,releases,token;bool borrowed;} fixture;
static void *acquire(void *p){fixture*f=p;f->acquires++;return f->fail==1?NULL:&f->token;}
static bool drain(void*p){fixture*f=p;f->drains++;f->borrowed=f->fail==2;return !f->borrowed;}
static bool work(void*p,void*owner){fixture*f=p;assert(owner==&f->token&&!f->borrowed);f->works++;f->borrowed=f->fail==3;return !f->borrowed;}
static bool restore(void*p){fixture*f=p;assert(!f->borrowed);f->restores++;f->borrowed=f->fail==4;return !f->borrowed;}
static void release(void*p,void*owner){fixture*f=p;assert(owner==&f->token&&!f->borrowed);f->releases++;}
int main(void){
    for(int fail=0;fail<=4;fail++) {
        fixture f={.fail=fail};diagnostic_lifecycle s={0};
        diagnostic_lifecycle_ops ops={&f,acquire,drain,work,restore,release};
        assert(diagnostic_lifecycle_run(NULL,&ops,true)==DIAG_LIFE_INVALID);
        assert(diagnostic_lifecycle_run(&s,NULL,true)==DIAG_LIFE_INVALID);
        for(int field=0;field<5;field++) {
            diagnostic_lifecycle_ops bad=ops;
            switch(field){case 0:bad.acquire=NULL;break;case 1:bad.drain=NULL;break;
                case 2:bad.work=NULL;break;case 3:bad.restore=NULL;break;default:bad.release=NULL;}
            assert(diagnostic_lifecycle_run(&s,&bad,true)==DIAG_LIFE_INVALID);
            assert(!s.owner&&!s.quarantined&&!f.acquires);
        }
        assert(diagnostic_lifecycle_run(&s,&ops,false)==DIAG_LIFE_REFUSED);
        s.owner=&f.token;
        assert(diagnostic_lifecycle_run(&s,&ops,true)==DIAG_LIFE_REFUSED);
        assert(!f.acquires&&!f.drains&&!f.works&&!f.restores&&!f.releases);
        s.owner=NULL; /* Test setup only: no acquired or borrowed resource. */
        diagnostic_lifecycle_result r=diagnostic_lifecycle_run(&s,&ops,true);
        assert(f.acquires==1);
        if(fail==0)assert(r==DIAG_LIFE_COMPLETE&&!s.owner&&!s.quarantined&&f.releases==1&&f.restores==1);
        else if(fail==1)assert(r==DIAG_LIFE_UNAVAILABLE&&!s.owner&&!f.drains&&!f.releases);
        else {
            assert(r==DIAG_LIFE_QUARANTINED&&s.owner==&f.token&&s.quarantined&&f.borrowed&&!f.releases);
            int calls=f.acquires+f.drains+f.works+f.restores;
            assert(diagnostic_lifecycle_run(&s,&ops,true)==DIAG_LIFE_QUARANTINED);
            assert(diagnostic_lifecycle_run(&s,NULL,false)==DIAG_LIFE_QUARANTINED);
            assert(calls==f.acquires+f.drains+f.works+f.restores&&!f.releases);
            assert(f.drains==1&&f.works==(fail>=3)&&f.restores==(fail==4));
        }
    }
    puts("PASS diagnostic lifecycle: validation, refusal, failure retention, success-only release");
}
