#ifndef _LOGGING_H_
#define _LOGGING_H_

#ifdef ENABLE_LOGGING
#define LOG(...)             \
    do                       \
    {                        \
        printf(__VA_ARGS__); \
        fflush(stdout);      \
    } while (0)
#else
#define LOG(...)
#endif

#endif