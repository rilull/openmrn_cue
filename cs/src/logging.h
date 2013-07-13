
#ifndef _LOGGING_H_
#define _LOGGING_H_

#ifndef __STDC_VERSION__
#define __STDC_VERSION__ 199901L
#endif

#include <stdio.h>

#define FATAL 0
#define ERROR 1
#define WARNING 2
#define INFO 3
#define VERBOSE 4

extern char logbuffer[256];

#define LOG(level, message...) do { if (LOGLEVEL >= level) { int sret = snprintf(logbuffer, sizeof(logbuffer), message); if (sret > (int)sizeof(logbuffer)) sret = sizeof(logbuffer); log_output(logbuffer, sret); } } while(0)


#ifndef LOGLEVEL
#define LOGLEVEL WARNING
#endif

#ifdef __cplusplus
extern "C" {
#endif
void log_output(char* buf, int size);
#ifdef __cplusplus
}
#endif




#endif // _LOGGING_H_