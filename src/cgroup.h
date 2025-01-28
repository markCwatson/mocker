#ifndef _CGROUP_H
#define _CGROUP_H

#include "common.h"

int setup_cgroup(pid_t child_pid);
int cleanup_cgroup(void);

#endif