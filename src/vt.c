#ifdef __linux__
	#define _XOPEN_SOURCE 600
#endif
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <termios.h>
#include <pwd.h>

#include <sys/ioctl.h>

#include <term/vt.h>
#include <term/log.h>

typedef struct dec_set_modes {
	uint32_t mode;
	uint32_t value;
} dec_modes_t;

static dec_modes_t dec_modes[] = {
	{ .mode = TERM_DECMODE_APP_CURSOR_KEYS, .value = TERM_MODE_APP_CURSOR_KEYS },
	{ .mode = TERM_DECMODE_MOUSE_X10, .value = TERM_MODE_MOUSE_X10 },
	{ .mode = TERM_DECMODE_SHOW_CURSOR, .value = TERM_MODE_SHOW_CURSOR },
	{ .mode = TERM_DECMODE_MOUSE_NORMAL, .value = TERM_MODE_MOUSE_NORMAL },
	{ .mode = TERM_DECMODE_MOUSE_BUTTON, .value = TERM_MODE_MOUSE_BUTTON },
	{ .mode = TERM_DECMODE_MOUSE_ANY, .value = TERM_MODE_MOUSE_MOTION_ALL },
	{ .mode = TERM_DECMODE_MOUSE_FOCUS, .value = TERM_MODE_MOUSE_FOCUS },
	{ .mode = TERM_DECMODE_MOUSE_SGR, .value = TERM_MODE_MOUSE_SGR },
	{ .mode = TERM_DECMODE_ALTSCREEN, .value = TERM_MODE_ALT_SCREEN },
	{ .mode = TERM_DECMODE_BRACKETED_PASTE, .value = TERM_MODE_BRACKTED_PASTE },
};

void vt_decsetmode(term_ctx_t *term, uint32_t mode) {
	for(uint32_t i = 0; i < sizeof(dec_modes) / sizeof(dec_modes[0]); ++i) {
		if(dec_modes[i].mode == mode) {
			term->mode |= dec_modes[i].value;
			return;
		}
	}
	log_debug("Unknown DECSET private mode: %d\n", mode);
}

void vt_decresetmode(term_ctx_t *term, uint32_t mode) {
	for(uint32_t i = 0; i < sizeof(dec_modes) / sizeof(dec_modes[0]); ++i) {
		if(dec_modes[i].mode == mode) {
			term->mode &= ~(dec_modes[i].value);
			return;
		}
	}
	log_debug("Unknown DECRESET private mode: %d\n", mode);
}

void vt_setcursor_pos(term_ctx_t *term, int32_t x, int32_t y) {
	if(x < 0) x = 0;
	if(y < 0) y = 0;
	if(x >= term->max_cols) x = term->max_cols - 1;
	if(y >= term->max_rows) y = term->max_rows - 1;

	term->col = x;
	term->row = y;
}

void vt_setcursor_shape(term_ctx_t *term, uint32_t shape) {
	static uint32_t cursors[] = { L'█', L'_', L'|' };
	if(shape < 3) {
		term->cursor = cursors[0];
	} else if(shape < 5) {
		term->cursor = cursors[1];
	} else {
		term->cursor = cursors[2];
	}
}

void vt_clear_screen(term_ctx_t *term) {
	for(int32_t y = 0; y < term->max_rows; y++) {
		for(int32_t x = 0; x < term->max_cols; x++) {
			term->screen[y][x].utf32 = ' ';
			term->screen[y][x].attributes = 0;
			term->screen[y][x].fg = term->fg;
			term->screen[y][x].bg = term->bg;
		}
	}
}

void vt_clear_line(term_ctx_t *term, uint32_t mode) {
	int32_t c = 0;
	int32_t max_c = 0;
	switch(mode) {
		case 0:
			c = term->col;
			max_c = term->max_cols;
			break;
		case 1:
			c = 0;
			max_c = term->col;
			break;
		case 2:
			c = 0;
			max_c = term->max_cols;
			break;
		default:
			log_warn("Unknown CSI[%dK(clear_line) sequence\n", mode);
			return;
	}

	for(; c < max_c; c++) {
		term->screen[term->row][c].utf32 = ' ';
	}
}

void vt_set_sgr(term_ctx_t *term, uint32_t *params, uint32_t pcount) {
	if(pcount == 0) {
		term->attributes = 0;
		term->fg = term->def_fg;
		term->bg = term->def_bg;
		return;
	}

	for(uint32_t p = 0; p < pcount; ++p) {
		uint32_t param = params[p];
		switch(param) {
			case 0:
				term->attributes = 0;
				term->fg = term->def_fg;
				term->bg = term->def_bg;
				break;
			case 1:
				term->attributes |= TERM_CELL_ATTRIBUTE_BOLD;
				break;
			case 2:
				term->attributes |= TERM_CELL_ATTRIBUTE_FAINT;
				break;
			case 3:
				term->attributes |= TERM_CELL_ATTRIBUTE_ITALIC;
				break;
			case 4:
				term->attributes |= TERM_CELL_ATTRIBUTE_UNDERLINE;
				break;
			case 5:
				term->attributes |= TERM_CELL_ATTRIBUTE_BLINK;
				break;
			case 7:
				term->attributes |= TERM_CELL_ATTRIBUTE_INVERSE;
				break;
			case 8:
				term->attributes |= TERM_CELL_ATTRIBUTE_INVIS;
				break;
			case 9:
				term->attributes |= TERM_CELL_ATTRIBUTE_CROSSED_OUT;
				break;
			case 21:
				term->attributes |= TERM_CELL_ATTRIBUTE_DBL_UNDERLINE;
				break;
			case 22:
				term->attributes &= ~(TERM_CELL_ATTRIBUTE_BOLD | TERM_CELL_ATTRIBUTE_FAINT);
				break;
			case 23:
				term->attributes &= ~(TERM_CELL_ATTRIBUTE_ITALIC);
				break;
			case 24:
				term->attributes &= ~(TERM_CELL_ATTRIBUTE_UNDERLINE);
				break;
			case 25:
				term->attributes &= ~(TERM_CELL_ATTRIBUTE_BLINK);
				break;
			case 27:
				term->attributes &= ~(TERM_CELL_ATTRIBUTE_INVERSE);
				break;
			case 28:
				term->attributes &= ~(TERM_CELL_ATTRIBUTE_INVIS);
				break;
			case 29:
				term->attributes &= ~(TERM_CELL_ATTRIBUTE_CROSSED_OUT);
				break;
			case 30:
			case 31:
			case 32:
			case 33:
			case 34:
			case 35:
			case 36:
			case 37:
				term->fg = term->colortable[param - 30];
				break;
			case 38:
				switch(params[p+1]) {
					case 2:
						term->fg = MAKE_ARGB(params[p + 2], params[p + 3], params[p + 4]);
						p+=4;
						break;
					case 5:
						term->fg = term->colortable[params[p + 2] & 0xff];
						p+=2;
						break;
					default:
						break;
				}
			break;
			case 39:
				term->fg = term->def_fg;
				break;
			case 40:
			case 41:
			case 42:
			case 43:
			case 44:
			case 45:
			case 46:
			case 47:
				term->bg = term->colortable[param - 40];
				break;
			case 48:
				switch(params[p+1]) {
					case 2:
						term->bg = MAKE_ARGB(params[p + 2], params[p + 3], params[p + 4]);
						p+=4;
						break;
					case 5:
						term->bg = term->colortable[params[p + 2] & 0xff];
						p+=2;
						break;
					default:
						break;
				}
				break;
			case 49:
				term->bg = term->def_bg;
				break;
			default:
				if(IN_RANGE(param, 90, 97)) {
					term->fg = term->colortable[param-90+8];
				} else if(IN_RANGE(param, 100, 107)) {
					term->fg = term->colortable[param-100+8];
				}
				break;
		}
	}
}

int getpty(int *parent, int *child) {
	int p = -1;
	int c = -1;
	char *child_path = NULL;

	if(parent == NULL || child == NULL) {
		errno = EINVAL;
		return -1;
	}

	p = posix_openpt(O_RDWR | O_NOCTTY);
	if(p < 0) {
		log_error("posix_openpt: %s\n", strerror(errno));
		return -1;
	}

	if(grantpt(p) < 0) {
		close(p);
		log_error("grantpt: %s\n", strerror(errno));
		return -1;
	}

	if(unlockpt(p) < 0) {
		close(p);
		log_error("unlockpt: %s\n", strerror(errno));
		return -1;
	}

#if defined(TIOCGPTPEER)
	/* If available try Linux's TIOCGPTPEER allocation method
	 * it seems ptsname method can be disabled by disabling
	 * CONFIG_UNIX98_PTYS kernel option. So try the TIOCGPTPEER
	 * first and fallback to ptsname if it doesn't work or isn't present
	 */
	c = ioctl(p, TIOCGPTPEER, O_RDWR | O_NOCTTY);
#endif
	if(c < 0) {
		child_path = ptsname(p);
		if(child_path == NULL) {
			close(p);
			log_error("ptsname: %s\n", strerror(errno));
			return -1;
		}
		c = open(child_path, O_RDWR | O_NOCTTY);
	}
	/*both approaches failed*/
	if(c < 0) {
		close(p);
		return -1;
	}
	
	*child = c;
	*parent = p;
	return 0;
}

static void child_proc_exec(const struct passwd *pw, char *shell, int fd) {
	unsetenv("COLUMNS");
	unsetenv("LINES");
	setenv("USER", pw->pw_name, 1);
	setenv("SHELL", shell, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("TERM", "project-term", 1);

	char *args[2] = { shell, NULL };

	if(setsid() < 0) {
		log_error("setsid: %s\n", strerror(errno));
		close(fd);
		exit(1);
	}

	if(ioctl(fd, TIOCSCTTY, NULL) == -1) {
		log_error("ioctl(TIOCSCTTY): %s\n", strerror(errno));
		close(fd);
		exit(1);
	}

	struct termios termios = { 0 };
	if(tcgetattr(fd, &termios) < 0) {
		log_error("tcgetattr: %s\n", strerror(errno));
		close(fd);
		exit(1);
	}
	termios.c_iflag |= IUTF8;

	if(tcsetattr(fd, TCSANOW, &termios) == -1) {
		log_error("tcsetattr: %s\n", strerror(errno));
		close(fd);
		exit(1);
	}

	dup2(fd, STDIN_FILENO);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	close(fd);

	if(execvp(shell, args) < 0) {
		log_error("execve: %s\n", strerror(errno));
		close(fd);
	}
	exit(1);
}

int forkshell(int parent, int child) {
	pid_t pid;
	char *shell = getenv("SHELL");
	const struct passwd *pw;

	pw = getpwuid(getuid());
	if(pw == NULL) {
		log_error("getpwuid failed: %s\n", strerror(errno));
		return -1;
	}

	if(shell == NULL && pw->pw_shell[0]) {
		shell = pw->pw_shell;
	}

	if(shell == NULL) {
		printf("Neither $SHELL or pw->pw_shell have a shell set\n");
		return -1;
	}
	pid = fork();

	if(pid < 0) {
		printf("error fork: %s\n", strerror(errno));
		return -1;
	} else if(pid == 0) {
		close(parent);
		child_proc_exec(pw, shell, child);
	}

	close(child);
	return 0;
}

void vt_fallback_color_table(term_ctx_t *term) {
	/*Standard Term Colors [30-37m*/
	term->colortable[0] = 0xff000000;
	term->colortable[1] = 0xffc00000;
	term->colortable[2] = 0xff00c000;
	term->colortable[3] = 0xffc0c000;
	term->colortable[4] = 0xff0000c0;
	term->colortable[5] = 0xffc000c0;
	term->colortable[6] = 0xff00c0c0;
	term->colortable[7] = 0xffe1e1e1;

	/*Bright Term Colors [90-97m*/
	term->colortable[8] = 0xff808080;
	term->colortable[9] = 0xffff0000;
	term->colortable[10] = 0xff00ff00;
	term->colortable[11] = 0xffffff00;
	term->colortable[12] = 0xff0000ff;
	term->colortable[13] = 0xffff00ff;
	term->colortable[14] = 0xff00ffff;
	term->colortable[15] = 0xffffffff;

	for(uint32_t r = 0; r < 6; r++) {
		for(uint32_t g = 0; g < 6; g++) {
			for(uint32_t b = 0; b < 6; b++) {
				term->colortable[16 + 36 * r + 6 * g + b] = MAKE_ARGB(r * 41, g * 41, b * 41);
			}
		}
	}

	for(uint32_t i = 232; i <= 255; ++i) {
		uint8_t p = (uint32_t)(10.625 * (i - 232));
		term->colortable[i] = MAKE_ARGB(p, p, p);
	}
}

void vt_csi_exec(term_ctx_t *term, const char *csi, uint32_t len) {
	uint8_t private = 0;
	uint8_t intermediate = 0;
	uint8_t mode = 0;
	uint32_t param_count = 0;
	uint32_t parameters[CSI_MAX_PARAM] = { 0 };
	uint32_t i = 0;

	if(csi[i] >= '<' && csi[i] <= '?') {
		private = csi[i];
		i++;
	}

	const char *p = &csi[i];
	while(p < csi + len) {
		char *np = NULL;
		if(param_count == CSI_MAX_PARAM) break;
		if(!IN_RANGE(p[0], '0', ';')) break;
		while(p[0] == ':' || p[0] == ';') {
			param_count++;
			p++;
		}

		parameters[param_count] = strtoul(p, &np, 10);
		p = np;
		param_count++;
		if(*p == ';' || *p == ':') p++;
	}

	if(IN_RANGE(p[0], 0x20, 0x2f)) {
		intermediate = p[0];
		p++;
	}
	mode = p[0];

	switch(private) {
		case '<':
			goto unknown_csi;
			break;
		case '=':
			goto unknown_csi;
			break;
		case '>':
			goto unknown_csi;
			break;
		case '?':
			switch(mode) {
				case 'h':
					vt_decsetmode(term, parameters[0]);
					break;
				case 'l':
					vt_decresetmode(term, parameters[0]);
					break;
				default:
					goto unknown_csi;
					break;
			}
			break;
		default:
			switch(mode) {
				case 'f':
				case 'H':
					parameters[0] = MAX(1, parameters[0]);
					parameters[1] = MAX(1, parameters[1]);
					vt_setcursor_pos(term, parameters[1]-1, parameters[0]-1);
					break;
				case 'J':
					switch (parameters[0]) {
						case 0:
							for(int32_t r = term->row; r < term->max_rows; r++) {
								for(int32_t c = r == 0 ? term->col : 0; c < term->max_cols; c++) {
									term->screen[r][c].utf32 = ' ';
									term->screen[r][c].bg = term->bg;
									term->screen[r][c].fg = term->fg;
								}
							}
							break;
						case 1:
							for(int32_t r = term->row; r > 0; r--) {
								for(int32_t c = r == 0 ? term->col : term->max_cols; c > 0; c--) {
									term->screen[r-1][c-1].utf32 = ' ';
									term->screen[r-1][c-1].bg = term->bg;
									term->screen[r-1][c-1].fg = term->fg;
								}
							}
							break;
						case 2:
							vt_clear_screen(term);
							break;
						case 3:
							log_warn("Erase Scrollback TODO\n");
							break;
						default:
							goto unknown_csi;
					}
					break;
				case 'K':
					vt_clear_line(term, parameters[0]);
					break;
				case 'n':
					switch(parameters[0]) {
						case 5: /*Device status report hardcode response 0 aka OK*/
							write(term->ptmx, "\x1b[0n", strlen("\x1b[0n"));
							break;
						default: goto unknown_csi;
					}
					break;
				case 'c':
					write(term->ptmx, "\x1b[?1;0c", strlen("\x1b[?1;0c"));
					break;
				case 'A':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, term->col, term->row - (parameters[0]));
					break;
				case 'B':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, term->col, term->row + (parameters[0]));
					break;
				case 'C':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, term->col + (parameters[0]), term->row);
					break;
				case 'D':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, term->col - (parameters[0]), term->row);
					break;
				case 'G':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, (parameters[0] - 1), term->row);
					break;
				case 'X':
					if(parameters[0] == 0) parameters[0]++;
					for(uint32_t k = term->col; k < term->col + parameters[0]; ++k) {
						term->screen[term->row][k].utf32 = ' ';
					}
					break;
				case 'd':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, term->col, parameters[0] - 1);
					break;
				case 'm':
					vt_set_sgr(term, parameters, param_count);
					break;
				case 'r':
					log_warn("TODO set scroll region\n");
					vt_setcursor_pos(term, 0, 0);
					break;
				case 'q':
					switch(intermediate) {
						case ' ':
							vt_setcursor_shape(term, parameters[0]);
							break;
					}
					break;
				default:
					goto unknown_csi;
			}
			break;
	}

	return;
unknown_csi:
	log_debug("Unknown CSI(%s):\n\tPrivate: %c(%x)\n\tIntermediate: %c(%x)\n\tMode: %c\n\tParameters: [", csi, private, private, intermediate, intermediate, mode);
	for(uint32_t p = 0; p < param_count; ++p) {
		log_printf_raw(TERM_LOG_LEVEL_DEBUG, " %d,", parameters[p]);
	}
	log_printf_raw(TERM_LOG_LEVEL_DEBUG, "]\n");
}

void vt_csi_handle(term_ctx_t *state) {
	char escape[128] = { 0 };
	uint32_t i = 0;

	do {
		read(state->ptmx, &escape[i], 1);
		i++;
	} while(i < 127 && !IN_RANGE(escape[i-1], 0x40, 0x7f));

	vt_csi_exec(state, escape, i);
}

void vt_strescape_handle(term_ctx_t *state, char byte) {
	char escape[4096] = { 0 };
	uint32_t i = 1;
	escape[0] = byte;
	do {
		read(state->ptmx, &escape[i], 1);
		if(escape[i] == '\a') break;
		if(strcmp(&escape[i-1], "\x1b\\") == 0) break;
		i++;
	} while(i < 4095);
}

void vt_escape_process(term_ctx_t *state) {
	char escape = 0;

	read(state->ptmx, &escape, 1);
	if(escape == '[') {
		vt_csi_handle(state);
		return;
	} else if(escape == ']' || escape == 'P' || escape == 'k') {
		vt_strescape_handle(state, escape);
		return;
	} else if(escape == '(') {
		read(state->ptmx, &escape, 1);
		return;
	} else if(escape == '=') {
		state->mode |= TERM_MODE_APP_KEYPAD;
	} else if(escape == '>') {
		state->mode &= ~(TERM_MODE_APP_KEYPAD);
	} else if(escape == '7') {
		state->saved_col = state->col;
		state->saved_row = state->row;
	} else if(escape == '8') {
		state->col = state->saved_col;
		state->row = state->saved_row;
	} else {
		log_debug("Unknown Escape Format: \\x1b%c\n", escape);
	}
}


