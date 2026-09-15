#ifndef KSN_CORE_FIXTURE_H
#define KSN_CORE_FIXTURE_H
#include "ksn_core.h"
/* Test-owned fixed blocks; init/reset must preserve these addresses. */
#define KSN_TEST_CORE(name, storage) \
    storage ksn_core_command_block name##_commands[2]; \
    storage ksn_core_text_block name##_text[2]; \
    storage ksn_core name={.state={.banks={ \
        {.commands=name##_commands[0].commands,.text=name##_text[0].bytes}, \
        {.commands=name##_commands[1].commands,.text=name##_text[1].bytes}}}}
#endif
