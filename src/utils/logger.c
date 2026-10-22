#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <pthread.h>
#include "utils/logger.h"

static FILE *log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void logger_init(const char *filename) {
    pthread_mutex_lock(&log_mutex);
    if (log_file) {
        fclose(log_file);
    }
    
    if (filename) {
        log_file = fopen(filename, "a");
    }
    pthread_mutex_unlock(&log_mutex);
}

void logger_log(log_level_t level, const char *file, int line, const char *fmt, ...) {
    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    char timestamp[32];
    va_list args;
    
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm);
    
    pthread_mutex_lock(&log_mutex);
    
    FILE *output = log_file ? log_file : stderr;
    
    fprintf(output, "%s [%s] [%s:%d] ", timestamp, level_str[level], file, line);
    
    va_start(args, fmt);
    vfprintf(output, fmt, args);
    va_end(args);
    
    fprintf(output, "\n");
    fflush(output);
    
    pthread_mutex_unlock(&log_mutex);
}

void logger_close(void) {
    pthread_mutex_lock(&log_mutex);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}
