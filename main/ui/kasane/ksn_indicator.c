#include "ksn_indicator.h"
ksn_result ksn_recording_emit(ksn_view *view,ksn_tx tx,ksn_recording_indicator state){
    if(!view||!tx.value||state.lit>6)return KSN_INVALID;
    if(!state.active)return KSN_OK;
    const ksn_rect clip={216,0,240,15};ksn_ref ref;
    ksn_draw draw={.kind=KSN_RECT,.bounds=clip,.clip=clip,.opacity=255,
        .data.shape={.color=0x100808ff}};
    ksn_result result=ksn_view_add(view,tx,&draw,&ref);if(result!=KSN_OK)return result;
    draw.bounds=(ksn_rect){226,4,234,12};draw.data.shape.color=0xe83030ff;
    result=ksn_view_add(view,tx,&draw,&ref);if(result!=KSN_OK)return result;
    for(unsigned cell=0;cell<6;cell++){
        int top=12-(int)(cell+1)*2;
        draw.bounds=(ksn_rect){219,top,223,top+1};
        draw.data.shape.color=cell<state.lit?
            (state.clipping&&cell==5?0xffb000ff:0xd2dee6ff):0x282424ff;
        result=ksn_view_add(view,tx,&draw,&ref);if(result!=KSN_OK)return result;
    }
    return KSN_OK;
}
