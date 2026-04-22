#ifdef __linux__
#define _POSIX_C_SOURCE 1
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdbool.h>

#include <soda-term/log.h>

static FILE *log_file;
static log_level_t log_level;
static bool only_level;

static const char *log_level_to_str(log_level_t level) {
	switch(level) {
		case TERM_LOG_LEVEL_INFO: return "Info";
		case TERM_LOG_LEVEL_DEBUG: return "Debug";
		case TERM_LOG_LEVEL_WARN: return "Warn";
		case TERM_LOG_LEVEL_ERROR: return "Error";
		default: return "Unknown";
	}
}

void log_set_level(log_level_t level, bool filter) {
	log_level = level;
	only_level = filter;
}

int log_open_file(const char *path) {
	FILE *fp = fopen(path, "w");
	if(!fp) {
		return -1;
	}

	log_file = fp;
	return 0;
}

int log_init(const char *path, log_level_t level, bool only_level) {
	if(log_open_file(path) == -1) {
		return -1;
	}

	log_set_level(level, only_level);
	return 0;
}

void log_set_file(FILE *fp) {
	log_file = fp;
}

void log_close_file(void) {
	if(log_file == stderr || log_file == stdout
		 || log_file == stdin || log_file == NULL) {
		return;
	}
	fclose(log_file);
	log_file = NULL;
}

void log_printf(const char *file, uint32_t line, log_level_t level, const char *fmt, ...) {
	bool colorize = false;
	char *no_color = getenv("NO_COLOR");
	char *force_color = getenv("FORCE_COLOR");
	va_list args;

	if(log_file == NULL) return;
	int fd = fileno(log_file);
	if(fd >= 0) {
		colorize = isatty(fd);
	}

	if(force_color != NULL && force_color[0] != '\0') {
		colorize = true;
	}

	if(no_color != NULL && no_color[0] != '\0') {
		colorize = false;
	}

	if((level >= log_level && only_level == false) ||
		 (log_level & level && only_level == true)) {
		fprintf(log_file, "%s%s%s(%d)%s: %s%s%s ",
						colorize ? "\x1b[1;35m" : "", file,
						colorize ? "\x1b[1;36m" : "", line,
						colorize ? "\x1b[0m" : "",
						colorize ? "\x1b[32m" : "", log_level_to_str(level),
						colorize ? "\x1b[0m" : "");

		va_start(args, fmt);
		vfprintf(log_file, fmt, args);
		va_end(args);
	}
}

void log_printf_raw(log_level_t level, const char *fmt, ...) {
	va_list args;

	if((level >= log_level && only_level == false) ||
		 (log_level & level && only_level == true)) {
		va_start(args, fmt);
		vfprintf(log_file, fmt, args);
		va_end(args);
	}
}
