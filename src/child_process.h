#ifndef _CHILD_PROCESS_
#define _CHILD_PROCESS_

struct child_args
{
    char **argv;
};

int child_function(void *arg);

#endif
