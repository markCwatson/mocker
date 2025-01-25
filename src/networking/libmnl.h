#ifndef _LIBMNL_H_
#define _LIBMNL_H_

#include "../common.h"

int move_veth_to_ns(const char *veth, pid_t pid);
int create_veth_pair(const char *host, const char *cont);

#endif