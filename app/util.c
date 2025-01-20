#include "util.h"

#include "common.h"

void disable_buffering(void)
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
}

void handle_error(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}
