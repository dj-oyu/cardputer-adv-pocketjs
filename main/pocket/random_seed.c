#include "random_stream.h"
#include "esp_random.h"
#include "sdkconfig.h"

/* CONFIG_POCKET_FIXED_RANDOM_SEED: measurement builds play the same game. */
uint32_t pocket_random_seed(void) {
#ifdef CONFIG_POCKET_FIXED_RANDOM_SEED
    return 0x5eed1234u;
#else
    return esp_random();
#endif
}
