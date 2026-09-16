#include "use_cases.h"
ksn_result pet_view_build(ksn_client c,ksn_resource image,pet_view *out) {
    ksn_tx tx;pet_view candidate;
    ksn_result r=c.ops->begin(c.ctx,KSN_REPLACE,&tx);if(r!=KSN_OK)return r;
    r=c.ops->background(c.ctx,tx,0x0b1727ff);if(r!=KSN_OK)goto fail;
    ksn_draw d={.kind=KSN_IMAGE,.bounds={27,29,91,93},.clip={7,25,112,109},.opacity=255};
    d.data.image.resource=image;
    r=c.ops->add(c.ctx,tx,&d,&candidate.pet);if(r!=KSN_OK)goto fail;
    d=(ksn_draw){.kind=KSN_RECT,.bounds={122,58,207,62},.clip={0,0,240,135},.opacity=255};
    d.data.shape.color=0x67dfc7ff;
    r=c.ops->add(c.ctx,tx,&d,&candidate.meter);if(r!=KSN_OK)goto fail;
    d=(ksn_draw){.kind=KSN_TEXT,.bounds={122,45,238,57},.clip={0,0,240,135},.opacity=255};
    d.data.text.utf8="FOOD 80";d.data.text.bytes=7;d.data.text.capacity=32;
    d.data.text.font=KSN_BODY;d.data.text.color=0xf5eedcff;
    r=c.ops->add(c.ctx,tx,&d,&candidate.label);if(r!=KSN_OK)goto fail;
    r=c.ops->end(c.ctx,tx);if(r!=KSN_OK)goto fail;
    *out=candidate;return KSN_OK;
fail:c.ops->abort(c.ctx,tx);return r;
}
ksn_result pet_view_update(ksn_client c,const pet_view *v,unsigned food) {
    if(food>100)return KSN_INVALID;
    ksn_tx tx;ksn_result r=c.ops->begin(c.ctx,KSN_PATCH,&tx);if(r!=KSN_OK)return r;
    ksn_change change={.property=KSN_SET_RECT,.value.rect={122,58,(int16_t)(122+(106*food+50)/100),62}};
    r=c.ops->change(c.ctx,tx,v->meter,&change);if(r!=KSN_OK)goto fail;
    char text[8]={'F','O','O','D',' ',0,0,0};unsigned n=5;
    if(food==100)text[n++]='1';
    if(food>=10)text[n++]=(char)('0'+food/10%10);
    text[n++]=(char)('0'+food%10);
    change=(ksn_change){.property=KSN_SET_TEXT};
    change.value.text.utf8=text;change.value.text.bytes=(uint16_t)n;
    r=c.ops->change(c.ctx,tx,v->label,&change);if(r!=KSN_OK)goto fail;
    r=c.ops->end(c.ctx,tx);if(r!=KSN_OK)goto fail;
    return KSN_OK;
fail:c.ops->abort(c.ctx,tx);return r;
}
ksn_result notice_view_show(ksn_client c,const char *text,uint16_t bytes) {
    ksn_tx tx;ksn_ref ref;ksn_result r=c.ops->begin(c.ctx,KSN_REPLACE,&tx);if(r!=KSN_OK)return r;
    ksn_draw d={.kind=KSN_TEXT,.bounds={8,8,232,22},.clip={0,0,240,48},.opacity=255};
    d.data.text.utf8=text;d.data.text.bytes=bytes;d.data.text.capacity=32;
    d.data.text.font=KSN_BODY;d.data.text.color=0xf5eedcff;
    r=c.ops->add(c.ctx,tx,&d,&ref);if(r==KSN_OK)r=c.ops->end(c.ctx,tx);
    if(r!=KSN_OK)c.ops->abort(c.ctx,tx);
    return r;
}
ksn_result modal_view_open(ksn_client c,const ksn_backdrop_api *b,ksn_backdrop_mode *actual) {
    ksn_backdrop_request request={8,1,0xdce8ef40,0x0b1727ff,2048};
    ksn_result r=b->capture(c.ctx,&request,actual);if(r!=KSN_OK)return r;
    ksn_tx tx;r=c.ops->begin(c.ctx,KSN_REPLACE,&tx);
    if(r!=KSN_OK){(void)b->release(c.ctx);return r;}
    ksn_draw d={.kind=KSN_ROUND_RECT,.bounds={28,27,212,109},.clip={0,0,240,135},.opacity=255};
    d.data.shape.color=0x1c3043d8;d.data.shape.radius=8;
    ksn_ref panel;r=c.ops->add(c.ctx,tx,&d,&panel);
    if(r==KSN_OK)r=c.ops->end(c.ctx,tx);
    if(r!=KSN_OK){c.ops->abort(c.ctx,tx);(void)b->release(c.ctx);}
    return r;
}
