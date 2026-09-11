#include "random_stream.h"
#include "esp_random.h"

uint32_t pocket_random_seed(void) { return esp_random(); }
