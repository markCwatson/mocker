#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <dirent.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>

#define CONTAINER_ROOT "/tmp/mocker"

struct child_args
{
    char **argv;
};

#endif