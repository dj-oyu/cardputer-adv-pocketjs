#ifndef KSN_PET_H
#define KSN_PET_H
#include "ksn_ports.h"
/* Validate once before registering. The borrowed PPT2 bytes remain immutable
 * and alive until this owner's resources are reset. No global pet selection. */
ksn_result ksn_pet_image(const uint8_t *data,size_t bytes,ksn_image_port *out);
#endif
