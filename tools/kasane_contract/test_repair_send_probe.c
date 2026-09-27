#include <assert.h>
#include <stdio.h>
#include "ksn_repair_send_probe.h"

int main(void){
    ksn_repair_send_probe probe={0};
    assert(ksn_repair_send_classify(0,0)==KSN_REPAIR_SEND_NORMAL);
    assert(ksn_repair_send_classify(1,1)==KSN_REPAIR_SEND_NORMAL);
    assert(ksn_repair_send_classify(1,2)==KSN_REPAIR_SEND_INJECT);
    assert(ksn_repair_send_classify(2,0)==KSN_REPAIR_SEND_RECOVER);
    assert(ksn_repair_send_classify(2,2)==KSN_REPAIR_SEND_RECOVER);
    ksn_repair_send_record(&probe,KSN_REPAIR_SEND_NORMAL,5300);
    ksn_repair_send_record(&probe,KSN_REPAIR_SEND_NORMAL,5400);
    ksn_repair_send_record(&probe,KSN_REPAIR_SEND_INJECT,5200);
    ksn_repair_send_record(&probe,KSN_REPAIR_SEND_RECOVER,5506);
    assert(probe.frames[KSN_REPAIR_SEND_NORMAL]==2);
    assert(probe.max_us[KSN_REPAIR_SEND_NORMAL]==5400);
    assert(probe.frames[KSN_REPAIR_SEND_INJECT]==1);
    assert(probe.max_us[KSN_REPAIR_SEND_INJECT]==5200);
    assert(probe.frames[KSN_REPAIR_SEND_RECOVER]==1);
    assert(probe.max_us[KSN_REPAIR_SEND_RECOVER]==5506);
    puts("repair send phase classifier: PASS");
    return 0;
}
