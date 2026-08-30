#ifndef NPK_LOG_H
#define NPK_LOG_H

#include "types.h"

typedef enum { LOG_TRACE, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_FATAL } log_level_t;
void log_init(void);
void log_message(log_level_t level, const char *component, const char *message);
void logf(log_level_t level, const char *component, const char *message, uint64_t value);
#define LOG_TRACEF(c,m,v) logf(LOG_TRACE,(c),(m),(v))
#define LOG_INFOF(c,m,v)  logf(LOG_INFO,(c),(m),(v))
#define LOG_WARNF(c,m,v)  logf(LOG_WARN,(c),(m),(v))
#define LOG_ERRORF(c,m,v) logf(LOG_ERROR,(c),(m),(v))

#endif
