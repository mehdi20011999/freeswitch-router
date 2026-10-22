#ifndef UTILS_LOGGER_H
#define UTILS_LOGGER_H

#include <stdio.h>

typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} log_level_t;

void logger_init(const char *filename);
void logger_log(log_level_t level, const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void logger_close(void);

#endif
