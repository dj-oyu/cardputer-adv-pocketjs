#include "pet/ksn_pet.h"
#include "kasane_pet_test_data.h"
ksn_result ksn_pet_builtin_image(ksn_image_port *out){
    return ksn_pet_image(pet_test_data,sizeof(pet_test_data),out);
}
