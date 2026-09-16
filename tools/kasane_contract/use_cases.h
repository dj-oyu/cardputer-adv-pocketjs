#ifndef KSN_USE_CASES_H
#define KSN_USE_CASES_H
#include "ksn_api.h"
#include "ksn_ports.h"
typedef struct { ksn_ref pet,meter,label; } pet_view;
ksn_result pet_view_build(ksn_client,ksn_resource,pet_view *);
ksn_result pet_view_update(ksn_client,const pet_view *,unsigned food);
ksn_result notice_view_show(ksn_client,const char *,uint16_t);
ksn_result modal_view_open(ksn_client,const ksn_backdrop_api *,ksn_backdrop_mode *);
#endif
