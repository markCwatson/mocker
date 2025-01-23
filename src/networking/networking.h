#ifndef _NETWORKING_H_
#define _NETWORKING_H_

#include "../common.h"

void cleanup_networking(void);
int setup_networking(pid_t child_pid);

#endif
