#ifndef DS_USE_CASES_H
#define DS_USE_CASES_H
#include "ds_api.h"
#include "ds_ports.h"
typedef struct { ds_ref pet,meter,label; } pet_view;
ds_result pet_view_build(ds_client,ds_resource,pet_view *);
ds_result pet_view_update(ds_client,const pet_view *,unsigned food);
ds_result notice_view_show(ds_client,const char *,uint16_t);
ds_result modal_view_open(ds_client,const ds_backdrop_api *,ds_backdrop_mode *);
#endif
