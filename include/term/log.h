#ifndef __TERM_LOG_H__
#define __TERM_LOG_H__

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint32_t log_level_t;

#define TERM_LOG_LEVEL_INFO (1)
#define TERM_LOG_LEVEL_DEBUG (1 << 1)
#define TERM_LOG_LEVEL_WARN (1 << 2)
#define TERM_LOG_LEVEL_ERROR (1 << 3)

void log_set_level(log_level_t level, bool filter);
int log_open_file(const char *path);
int log_init(const char *path, log_level_t level, bool only_level);
void log_set_file(FILE *fp);
void log_close_file(void);
void log_printf(const char *file, uint32_t line, log_level_t level, const char *fmt, ...);
void log_printf_raw(log_level_t level, const char *fmt, ...);

#define log_info(...) log_printf(__FILE__, __LINE__, TERM_LOG_LEVEL_INFO, __VA_ARGS__)
#define log_debug(...) log_printf(__FILE__, __LINE__, TERM_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define log_warn(...) log_printf(__FILE__, __LINE__, TERM_LOG_LEVEL_WARN, __VA_ARGS__)
#define log_error(...) log_printf(__FILE__, __LINE__, TERM_LOG_LEVEL_ERROR, __VA_ARGS__)


#endif
