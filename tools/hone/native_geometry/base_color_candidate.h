#ifndef HONE_BASE_COLOR_CANDIDATE_H
#define HONE_BASE_COLOR_CANDIDATE_H
#include "alpha_factor_candidate.h"
/* Three independent scalar programs yield a staged linear RGB candidate.
 * The caller supplies the actual fragment-local coordinate and owns scratch.
 * This is not Principled lighting, alpha compositing, or target admission. */
static inline int hone_base_color(const hone_alpha_program channels[3],const float xyz[3],
 const double *driver,size_t driver_count,float *scratch,size_t capacity,float out[3]){
 if(!channels||!out)return 0;float rgb[3];
 for(unsigned k=0;k<3;k++)if(!hone_alpha_factor(&channels[k],xyz,driver,driver_count,scratch,capacity,&rgb[k]))return 0;
 for(unsigned k=0;k<3;k++)out[k]=rgb[k];return 1;
}
#endif
