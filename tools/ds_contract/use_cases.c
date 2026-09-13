#include "use_cases.h"
ds_result pet_view_build(ds_client c,ds_resource image,pet_view *out) {
    ds_tx tx;pet_view candidate;
    ds_result r=c.ops->begin(c.ctx,DS_REPLACE,&tx);if(r!=DS_OK)return r;
    r=c.ops->background(c.ctx,tx,0x0b1727ff);if(r!=DS_OK)goto fail;
    ds_draw d={.kind=DS_IMAGE,.bounds={27,29,91,93},.clip={7,25,112,109},.opacity=255};
    d.data.image.resource=image;
    r=c.ops->add(c.ctx,tx,&d,&candidate.pet);if(r!=DS_OK)goto fail;
    d=(ds_draw){.kind=DS_RECT,.bounds={122,58,207,62},.clip={0,0,240,135},.opacity=255};
    d.data.shape.color=0x67dfc7ff;
    r=c.ops->add(c.ctx,tx,&d,&candidate.meter);if(r!=DS_OK)goto fail;
    d=(ds_draw){.kind=DS_TEXT,.bounds={122,45,238,57},.clip={0,0,240,135},.opacity=255};
    d.data.text.utf8="FOOD 80";d.data.text.bytes=7;d.data.text.capacity=32;
    d.data.text.font=DS_BODY;d.data.text.color=0xf5eedcff;
    r=c.ops->add(c.ctx,tx,&d,&candidate.label);if(r!=DS_OK)goto fail;
    r=c.ops->end(c.ctx,tx);if(r!=DS_OK)goto fail;
    *out=candidate;return DS_OK;
fail:c.ops->abort(c.ctx,tx);return r;
}
ds_result pet_view_update(ds_client c,const pet_view *v,unsigned food) {
    if(food>100)return DS_INVALID;
    ds_tx tx;ds_result r=c.ops->begin(c.ctx,DS_PATCH,&tx);if(r!=DS_OK)return r;
    ds_change change={.property=DS_SET_RECT,.value.rect={122,58,(int16_t)(122+(106*food+50)/100),62}};
    r=c.ops->change(c.ctx,tx,v->meter,&change);if(r!=DS_OK)goto fail;
    char text[8]={'F','O','O','D',' ',0,0,0};unsigned n=5;
    if(food==100)text[n++]='1';
    if(food>=10)text[n++]=(char)('0'+food/10%10);
    text[n++]=(char)('0'+food%10);
    change=(ds_change){.property=DS_SET_TEXT};
    change.value.text.utf8=text;change.value.text.bytes=(uint16_t)n;
    r=c.ops->change(c.ctx,tx,v->label,&change);if(r!=DS_OK)goto fail;
    r=c.ops->end(c.ctx,tx);if(r!=DS_OK)goto fail;
    return DS_OK;
fail:c.ops->abort(c.ctx,tx);return r;
}
ds_result notice_view_show(ds_client c,const char *text,uint16_t bytes) {
    ds_tx tx;ds_ref ref;ds_result r=c.ops->begin(c.ctx,DS_REPLACE,&tx);if(r!=DS_OK)return r;
    ds_draw d={.kind=DS_TEXT,.bounds={8,8,232,22},.clip={0,0,240,48},.opacity=255};
    d.data.text.utf8=text;d.data.text.bytes=bytes;d.data.text.capacity=32;
    d.data.text.font=DS_BODY;d.data.text.color=0xf5eedcff;
    r=c.ops->add(c.ctx,tx,&d,&ref);if(r==DS_OK)r=c.ops->end(c.ctx,tx);
    if(r!=DS_OK)c.ops->abort(c.ctx,tx);
    return r;
}
ds_result modal_view_open(ds_client c,const ds_backdrop_api *b,ds_backdrop_mode *actual) {
    ds_backdrop_request request={8,1,0xdce8ef40,0x0b1727ff,2048};
    ds_result r=b->capture(c.ctx,&request,actual);if(r!=DS_OK)return r;
    ds_tx tx;r=c.ops->begin(c.ctx,DS_REPLACE,&tx);
    if(r!=DS_OK){(void)b->release(c.ctx);return r;}
    ds_draw d={.kind=DS_ROUND_RECT,.bounds={28,27,212,109},.clip={0,0,240,135},.opacity=255};
    d.data.shape.color=0x1c3043d8;d.data.shape.radius=8;
    ds_ref panel;r=c.ops->add(c.ctx,tx,&d,&panel);
    if(r==DS_OK)r=c.ops->end(c.ctx,tx);
    if(r!=DS_OK){c.ops->abort(c.ctx,tx);(void)b->release(c.ctx);}
    return r;
}
