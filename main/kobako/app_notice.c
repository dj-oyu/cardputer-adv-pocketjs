#include "app_notice.h"
ksn_result ksn_notice_emit(ksn_view *view,ksn_tx tx,const sys_notice *notice,ksn_resource pet,uint16_t variant){
    if(!view||!tx.value)return KSN_INVALID;
    if(!notice)return KSN_OK;
    if(notice->phase!=NOTICE_ACTIVE)return KSN_INVALID;
    uint16_t bytes=0;while(bytes<SYS_NOTICE_LABEL&&notice->label[bytes])bytes++;
    if(!bytes||notice->label[bytes])return KSN_INVALID;
    const ksn_rect clip={0,0,240,48};ksn_ref ref;
    ksn_draw draw={.kind=KSN_RECT,.bounds=clip,.clip=clip,.opacity=255,.data.shape={.color=0x080c20ff}};
    ksn_result result=ksn_view_add(view,tx,&draw,&ref);if(result!=KSN_OK)return result;
    draw.bounds=(ksn_rect){0,46,240,48};draw.data.shape.color=0x3edcd0ff;
    result=ksn_view_add(view,tx,&draw,&ref);if(result!=KSN_OK)return result;
    if(pet.value){
        draw=(ksn_draw){.kind=KSN_IMAGE,.bounds={5,5,37,37},.clip=clip,.opacity=255,
            .data.image={.resource=pet,.variant=variant,.frame=3,.scale=KSN_IMAGE_HALF}};
        result=ksn_view_add(view,tx,&draw,&ref);if(result!=KSN_OK)return result;
    }
    draw=(ksn_draw){.kind=KSN_TEXT,.bounds={42,10,238,20},.clip=clip,.opacity=255,
        .data.text={.utf8=notice->label,.bytes=bytes,.capacity=SYS_NOTICE_LABEL,.font=KSN_CAPTION,.color=0xf0f7e6ff}};
    result=ksn_view_add(view,tx,&draw,&ref);if(result!=KSN_OK)return result;
    static const char hint[]="ENTER OK > SNOOZE";
    draw.bounds=(ksn_rect){42,29,238,39};draw.data.text.utf8=hint;
    draw.data.text.bytes=draw.data.text.capacity=sizeof(hint)-1;draw.data.text.color=0x63d6ddff;
    return ksn_view_add(view,tx,&draw,&ref);
}
