#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void signal_sim_start(void);
void signal_sim_stop(void);
bool signal_sim_is_active(void);

#ifdef __cplusplus
}
#endif
