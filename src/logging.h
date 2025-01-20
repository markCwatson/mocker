#ifndef _LOGGING_H_
#define _LOGGING_H_


#ifdef ENABLE_LOGGING
#define LOG(...) printf(__VA_ARGS__)
#else
#define LOG(...)
#endif

#endif