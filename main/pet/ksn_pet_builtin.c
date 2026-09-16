#include "ksn_pet.h"
extern const uint8_t _binary_pets_compact_bin_start[];
extern const uint8_t _binary_pets_compact_bin_end[];
ksn_result ksn_pet_builtin_image(ksn_image_port *out){
    return ksn_pet_image(_binary_pets_compact_bin_start,
                        (size_t)(_binary_pets_compact_bin_end-_binary_pets_compact_bin_start),out);
}
