#ifdef __linux__
	#define _XOPEN_SOURCE 600
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <termios.h>
#include <pwd.h>
#include <poll.h>

#include <sys/ioctl.h>

#include <soda-term/vt.h>
#include <soda-term/log.h>

void vt_setcursor_pos(vt_ctx_t *term, int32_t x, int32_t y) {
	if(term->term_mode & TERM_MODE_ORIGIN) {
		y += term->top;
		if(y >= term->bottom) y = term->bottom;
	} else {
		if(y < 0) y = 0;
		if(y >= term->max_rows) y = term->max_rows - 1;
	}
	if(x < 0) x = 0;
	if(x >= term->max_cols) x = term->max_cols - 1;
	
	term->flags &= ~TERM_FLAGS_NEED_WRAP;
	term->cursor_pos.x = x;
	term->cursor_pos.y = y;
}

void vt_decquery_mode(vt_ctx_t *term, uint32_t mode) {
	char buffer[64] = { 0 };
	log_debug("DECRQM unknown mode: %d\n", mode);
	snprintf(buffer, 64, "\x1b[?%d;%d$y", mode, 0);
	write(term->ptmx, buffer, strlen(buffer));
}

void vt_decsetmode(vt_ctx_t *term, uint32_t mode) {
	switch(mode) {
		case TERM_DECMODE_APP_CURSOR_KEYS:
			term->term_mode |= TERM_MODE_APP_CURSOR_KEYS;
			break;
		case TERM_DECMODE_COLUMN_MODE:
			vt_clear_screen(term);
			vt_setcursor_pos(term, 0, 0);
			break;
		case TERM_DECMODE_SMOOTH_SCROLL_MODE:
			break;
		case TERM_DECMODE_REVERSE_VIDEO_MODE:
			term->term_mode |= TERM_MODE_REVERSE_VIDEO;
			for(int32_t r = 0; r < term->max_rows; ++r) {
				for(int32_t c = 0; c < term->max_cols; ++c) {
					if(term->screen[r][c].fg == term->def_fg) {
						term->screen[r][c].bg = term->def_fg;
					}
					if(term->screen[r][c].bg == term->def_bg) {
						term->screen[r][c].fg = term->def_bg;
					}
					term->fg = term->def_bg;
					term->bg = term->def_fg;
				}
			}
			break;
		case TERM_DECMODE_ORIGIN_MODE:
			term->term_mode |= TERM_MODE_ORIGIN;
			vt_setcursor_pos(term, 0, 0);
			break;
		case TERM_DECMODE_AUTO_WRAP:
			term->term_mode |= TERM_MODE_WRAPAROUND;
			break;
		case TERM_DECMODE_AUTO_REPEAT:
			break;
		case TERM_DECMODE_MOUSE_X10:
			term->pointer_mode |= TERM_MODE_MOUSE_X10;
			break;
		case TERM_DECMODE_SHOW_CURSOR:
			term->term_mode |= TERM_MODE_SHOW_CURSOR;
			break;
		case TERM_DECMODE_MOUSE_NORMAL:
			term->pointer_mode |= TERM_MODE_MOUSE_NORMAL;
			break;
		case TERM_DECMODE_MOUSE_BUTTON:
			term->pointer_mode |= TERM_MODE_MOUSE_MOTION_ALL;
			break;
		case TERM_DECMODE_MOUSE_FOCUS:
			term->pointer_mode |= TERM_MODE_MOUSE_FOCUS;
			break;
		case TERM_DECMODE_MOUSE_SGR:
			term->pointer_mode |= TERM_MODE_MOUSE_SGR;
			break;
		case TERM_DECMODE_ALTSCREEN:
			term->term_mode |= TERM_MODE_ALT_SCREEN;
			break;
		case TERM_DECMODE_BRACKETED_PASTE:
			term->term_mode |= TERM_MODE_BRACKTED_PASTE;
			break;
		case TERM_DECMODE_IN_BAND_RESIZE:
			term->term_mode |= TERM_MODE_RESIZE_NOTIFY;
			break;
		default:
			log_debug("Unknown DECSET private mode: %d\n", mode);
			break;
	}
}

void vt_decresetmode(vt_ctx_t *term, uint32_t mode) {
	switch(mode) {
		case TERM_DECMODE_APP_CURSOR_KEYS:
			term->term_mode &= ~TERM_MODE_APP_CURSOR_KEYS;
			break;
		case TERM_DECMODE_VT52_MODE:
			term->term_mode |= TERM_MODE_VT52;
			break;
		case TERM_DECMODE_COLUMN_MODE:
			vt_clear_screen(term);
			vt_setcursor_pos(term, 0, 0);
			break;
		case TERM_DECMODE_SMOOTH_SCROLL_MODE:
			break;
		case TERM_DECMODE_REVERSE_VIDEO_MODE:
			term->term_mode &= ~TERM_MODE_REVERSE_VIDEO;
			for(int32_t r = 0; r < term->max_rows; ++r) {
				for(int32_t c = 0; c < term->max_cols; ++c) {
					if(term->screen[r][c].fg == term->def_bg) {
						term->screen[r][c].fg = term->def_fg;
					}
					if(term->screen[r][c].bg == term->def_fg) {
						term->screen[r][c].bg = term->def_bg;
					}
				}
			}
			term->fg = term->def_fg;
			term->bg = term->def_bg;
			break;
		case TERM_DECMODE_ORIGIN_MODE:
			term->term_mode &= ~TERM_MODE_ORIGIN;
			vt_setcursor_pos(term, 0, 0);
			break;
		case TERM_DECMODE_AUTO_WRAP:
			term->term_mode &= ~TERM_MODE_WRAPAROUND;
			break;
		case TERM_DECMODE_AUTO_REPEAT:
			break;
		case TERM_DECMODE_MOUSE_X10:
			term->pointer_mode &= ~TERM_MODE_MOUSE_X10;
			break;
		case TERM_DECMODE_SHOW_CURSOR:
			term->term_mode &= ~TERM_MODE_SHOW_CURSOR;
			break;
		case TERM_DECMODE_MOUSE_NORMAL:
			term->pointer_mode &= ~TERM_MODE_MOUSE_NORMAL;
			break;
		case TERM_DECMODE_MOUSE_BUTTON:
			term->pointer_mode &= ~TERM_MODE_MOUSE_MOTION_ALL;
			break;
		case TERM_DECMODE_MOUSE_FOCUS:
			term->pointer_mode &= ~TERM_MODE_MOUSE_FOCUS;
			break;
		case TERM_DECMODE_MOUSE_SGR:
			term->pointer_mode &= ~TERM_MODE_MOUSE_SGR;
			break;
		case TERM_DECMODE_ALTSCREEN:
			term->term_mode &= ~TERM_MODE_ALT_SCREEN;
			break;
		case TERM_DECMODE_BRACKETED_PASTE:
			term->term_mode &= ~TERM_MODE_BRACKTED_PASTE;
			break;
		case TERM_DECMODE_IN_BAND_RESIZE:
			term->term_mode &= ~TERM_MODE_RESIZE_NOTIFY;
			break;
		default:
			log_debug("Unknown DECRESET private mode: %d\n", mode);
			break;
	}
}

void vt_setcursor_shape(vt_ctx_t *term, uint32_t shape) {
	static uint32_t cursors[] = { L'█', L'_', L'|' };
	if(shape < 3) {
		term->cursor.utf32 = cursors[0];
	} else if(shape < 5) {
		term->cursor.utf32 = cursors[1];
	} else {
		term->cursor.utf32 = cursors[2];
	}
}

uint32_t utf8_get_length(uint8_t b1) {
	if(b1 < 0x80) {
		return 1;
	} else if((b1 & 0xe0) == 0xc0) {
		return 2;
	} else if((b1 & 0xf0) == 0xe0) {
		return 3;
	} else if((b1 & 0xf8) == 0xf0) {
		return 4;
	}

	return 0;
}

uint32_t utf8_to_utf32(const uint8_t *utf8) {
	if(utf8[0] < 0x80) {
		return (uint32_t)utf8[0];
	} else if((utf8[0] & 0xe0) == 0xc0) {
		return (((uint32_t)(utf8[0] & 0x1f) << 6) | (uint32_t)(utf8[1] & 0x3f));
	} else if((utf8[0] & 0xf0) == 0xe0) {
		return (((uint32_t)(utf8[0] & 0x0f) << 12) | ((uint32_t)(utf8[1] & 0x3f) << 6) | (uint32_t)(utf8[2] & 0x3f));
	} else if((utf8[0] & 0xf8) == 0xf0) {
		return (((uint32_t)(utf8[0] & 0x07) << 18) | ((uint32_t)(utf8[1] & 0x3f) << 12) | ((uint32_t)(utf8[2] & 0x3f) << 6) | ((uint32_t)utf8[3] & 0x3f));
	}
	return 0;
}

uint32_t vt_read_utf32(vt_ctx_t *vt) {
	int fd = vt->ptmx;
	uint8_t b1 = 0;
	uint8_t extbytes[5] = { 0 };
	const char *graphics_charset[] = {
										" ", "♦", "▒", "␉", "␌", "␍",
										"␊", "°", "±", "␤", "␋", "┘",
										"┐", "┌", "└", "┼", "⎺", "⎻", "─",
										"⎼", "⎽", "├", "┤", "┴", "┬", "│",
										"≤", "≥", "π", "≠", "£", "·"
	};

	read(fd, &b1, 1);

	if(IN_RANGE(b1, '_', '~') && vt->charset) {
		return utf8_to_utf32((uint8_t*)graphics_charset[b1 - '_']);
	}

	if(CTRL_CODE(b1)) {
		return b1;
	}

	uint32_t len = utf8_get_length(b1);
	if(len == 0) {
		log_warn("Invalid UTF-8 sequence returning as CTRL char %x\n", b1);
		return b1;
	}

	extbytes[0] = b1;
	read(fd, &extbytes[1], len - 1);
	return utf8_to_utf32(extbytes);
}

void vt_process_char(vt_ctx_t *vt, utf32_t c) {
	uint8_t byte = 0;
	if(CTRL_CODE(c)) {
		vt_handle_ctrl_code(vt, c);
		return;
	}

	if(vt->state == TERM_STATE_ESC_START && vt->term_mode & TERM_MODE_VT52) {
		vt52_escape_process(vt, c);
		vt->state = TERM_STATE_GROUND;
		return;
	}

	switch (vt->state) {
		case TERM_STATE_ESC_START:
			switch(c) {
				case '[':
					vt->state = TERM_STATE_CSI_SEQUENCE;
					break;
				case '(':
					read(vt->ptmx, &byte, 1);
					break;
				case '=':
					vt->state = TERM_STATE_GROUND;
					vt->term_mode |= TERM_MODE_APP_KEYPAD;
					break;
				case 'P':
					vt->state = TERM_STATE_DCS_SEQUENCE;
					break;
				case 'E':
					vt_scroll(vt);
					vt->cursor_pos.x = 0;
					vt->state = TERM_STATE_GROUND;
					break;
				case 'D':
					vt->cursor_pos.y++;;
					if(vt->cursor_pos.y >= vt->max_rows) {
						for(int32_t i = 1; i < vt->max_rows; i++) {
							memcpy(vt->screen[i-1], vt->screen[i], vt->max_cols * sizeof(vt_cell_t));
						}
						vt->cursor_pos.y = vt->max_rows - 1;
						for(int32_t i = 0; i < vt->max_cols; i++) {
							vt->screen[vt->cursor_pos.y][i].utf32 = ' ';
						}
					}
					vt->state = TERM_STATE_GROUND;
					break;
				case 'M':
					vt->cursor_pos.y--;
					if(vt->cursor_pos.y < 0) {
						for(int32_t i = vt->max_rows - 1; i > 0; i--) {
							memcpy(vt->screen[i], vt->screen[i-1], vt->max_cols * sizeof(vt_cell_t));
						}
						vt->cursor_pos.y = 0;
						for(int32_t i = 0; i < vt->max_cols; i++) {
							vt->screen[0][i].utf32 = ' ';
						}
					}
					vt->state = TERM_STATE_GROUND;
				break;
				case '#':
					read(vt->ptmx, &byte, 1);
					if(byte == '8') {
						for(int32_t y = 0; y < vt->max_rows; y++) {
							for(int32_t x = 0; x < vt->max_cols; x++) {
								vt->screen[y][x].utf32 = 'E';
							}
						}
					} else {
						log_debug("\\x1b#%c\n", byte);
					}
					vt->state = TERM_STATE_GROUND;
					break;
				case '%':
					read(vt->ptmx, &byte, 1);
					log_debug("\\x1b%%%c\n", byte);
					vt->state = TERM_STATE_GROUND;
					break;
				case '>':
					vt->state = TERM_STATE_GROUND;
					vt->term_mode &= ~TERM_MODE_APP_KEYPAD;
					break;
				default:
					log_warn("Unhandled escape %c(%x)\n", c, c);
					vt->state = 0;
					break;
			}
			break;
		case TERM_STATE_DCS_SEQUENCE: {
			uint32_t i = 2;
			memset(vt->strescape_buffer, 0, 4096);
			read(vt->ptmx, vt->strescape_buffer, 2);
			while(vt->strescape_buffer[i - 2] != '\x1b' && vt->strescape_buffer[i - 1] != '\\') {
				read(vt->ptmx, &vt->strescape_buffer[i], 1);
				if(vt->strescape_buffer[i] == '\a') break;
				i++;
				if(i >= DCS_BUFFER_LEN - 1) break;
			}
			log_debug("%s\n", vt->strescape_buffer);
			vt->state = TERM_STATE_GROUND;
			break;
		}
		case TERM_STATE_CSI_SEQUENCE:
			vt->csi_buffer[vt->csi_len] = c;
			vt->csi_len++;
			if(IN_RANGE(vt->csi_buffer[vt->csi_len-1], 0x40, 0x7f)) {
				vt_csi_exec(vt, (char*)vt->csi_buffer, vt->csi_len);
				vt->state = 0;	
				if(vt->csi_len >= CSI_BUFFER_LEN - 1) break;
			}
			break;
		case TERM_STATE_GROUND:
		default:
			if(vt->cursor_pos.x >= vt->max_cols || vt->flags & TERM_FLAGS_NEED_WRAP) {
				if(vt->term_mode & TERM_MODE_WRAPAROUND) {
					vt_scroll(vt);
				}
				vt->cursor_pos.x = 0;
				vt->flags &= ~(TERM_FLAGS_NEED_WRAP);
			}
			vt->screen[vt->cursor_pos.y][vt->cursor_pos.x].utf32 = c;
			vt->screen[vt->cursor_pos.y][vt->cursor_pos.x].fg = vt->fg;
			vt->screen[vt->cursor_pos.y][vt->cursor_pos.x].bg = vt->bg;
			vt->screen[vt->cursor_pos.y][vt->cursor_pos.x].attributes = vt->attributes;
			vt->last_char = c;
			vt->cursor_pos.x++;
			if(vt->cursor_pos.x == vt->max_cols) {
				vt->cursor_pos.x = vt->max_cols - 1;
				vt->flags |= TERM_FLAGS_NEED_WRAP;
			}
			break;
	}
}

int vt_event(vt_ctx_t *vt) {
	struct pollfd pfd = { vt->ptmx, POLLIN, 0 };
	uint32_t c = 0;
	while(poll(&pfd, 1, 0)) {
		if(pfd.revents & POLLHUP || pfd.revents & POLLERR) {
			log_error("poll: %s\n", strerror(errno));
			return -1;
		} else if(pfd.revents & POLLIN) {
			c = vt_read_utf32(vt);
			vt_process_char(vt, c);
		}
	}

	return 0;
}

void vt_clear_screen(vt_ctx_t *term) {
	for(int32_t y = 0; y < term->max_rows; y++) {
		for(int32_t x = 0; x < term->max_cols; x++) {
			term->screen[y][x].utf32 = ' ';
			term->screen[y][x].attributes = 0;
			term->screen[y][x].fg = term->fg;
			term->screen[y][x].bg = term->bg;
		}
	}
}

void vt_report_cursor(vt_ctx_t *vt) {
	char buffer[CSI_BUFFER_LEN] = { 0 };

	snprintf(buffer, CSI_BUFFER_LEN, "\x1b[%d;%dR", vt->cursor_pos.y+1, vt->cursor_pos.x+1);
	write(vt->ptmx, buffer, strlen(buffer));
}

void vt_clear_line(vt_ctx_t *term, uint32_t mode) {
	int32_t c = 0;
	int32_t max_c = 0;
	switch(mode) {
		case 0:
			c = term->cursor_pos.x;
			max_c = term->max_cols;
			break;
		case 1:
			c = 0;
			max_c = term->cursor_pos.x + 1;
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
		term->screen[term->cursor_pos.y][c].utf32 = ' ';
	}
}

void vt_set_sgr(vt_ctx_t *term, uint32_t *params, uint32_t pcount) {
	if(pcount == 0) {
		term->attributes = 0;
		term->fg = term->term_mode & TERM_MODE_REVERSE_VIDEO ? term->def_bg : term->def_fg;
		term->bg = term->term_mode & TERM_MODE_REVERSE_VIDEO ? term->def_fg : term->def_bg;
		return;
	}

	for(uint32_t p = 0; p < pcount; ++p) {
		uint32_t param = params[p];
		switch(param) {
			case 0:
				term->attributes = 0;
				term->fg = term->term_mode & TERM_MODE_REVERSE_VIDEO ? term->def_bg : term->def_fg;
				term->bg = term->term_mode & TERM_MODE_REVERSE_VIDEO ? term->def_fg : term->def_bg;
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

void vt_fallback_color_table(vt_ctx_t *term) {
	term->colortable[0] = 0xff000000;
	term->colortable[1] = 0xffc00000;
	term->colortable[2] = 0xff00c000;
	term->colortable[3] = 0xffc0c000;
	term->colortable[4] = 0xff0000c0;
	term->colortable[5] = 0xffc000c0;
	term->colortable[6] = 0xff00c0c0;
	term->colortable[7] = 0xffe1e1e1;

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


void vt_csi_exec(vt_ctx_t *term, const char *csi, uint32_t len) {
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
			switch(mode) {
				case 'c':
					if(parameters[0] == 0) {
						const char *tertiary_da = "\x1bP!|00000000\x1b\\";
						write(term->ptmx, tertiary_da, strlen(tertiary_da));
					}
					break;
				default:
					goto unknown_csi;
			}
			break;
		case '>':
			switch(mode) {
				case 'q':
					if(parameters[0] == 0) {
						write(term->ptmx, "\x1bP>|project-term 0.0.1\x1b\\", strlen("\x1bP>|project-term 0.0.1\x1b\\"));
					}
					break;
				case 'c':
					if(parameters[0] == 0) {
						const char *secondary_da = "\x1b[>65;000001;0c";
						write(term->ptmx, secondary_da, strlen(secondary_da));
					}
					break;
				default:
					goto unknown_csi;
			}
			break;
		case '?':
			switch(mode) {
				case 'h':
					vt_decsetmode(term, parameters[0]);
					break;
				case 'l':
					vt_decresetmode(term, parameters[0]);
					break;
				case 'p':
					switch(intermediate) {
						case '$':
							vt_decquery_mode(term, parameters[0]);
							break;
						default:
							goto unknown_csi;
							break;
					}
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
						vt_setcursor_pos(term, parameters[1]-1, (parameters[0]-1));
					break;
				case 'J':
					switch (parameters[0]) {
						case 0:
							for(int32_t r = term->cursor_pos.y; r < term->max_rows; r++) {
								for(int32_t c = r == term->cursor_pos.y ? term->cursor_pos.x : 0; c < term->max_cols; c++) {
									term->screen[r][c].utf32 = ' ';
									term->screen[r][c].bg = term->bg;
									term->screen[r][c].fg = term->fg;
								}
							}
							break;
						case 1:
							for(int32_t r = term->cursor_pos.y + 1; r > 0; r--) {
								for(int32_t c = (r - 1) == term->cursor_pos.y ? term->cursor_pos.x + 1 : term->max_cols; c > 0; c--) {
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
						case 5:
							write(term->ptmx, "\x1b[0n", strlen("\x1b[0n"));
							break;
						case 6:
							vt_report_cursor(term);
							break;
						default: goto unknown_csi;
					}
					break;
				case 'c':
					write(term->ptmx, "\x1b[?64c", strlen("\x1b[?64c"));
					break;
				case 't':
					log_info("ignoring window parameters: %s\n", csi);
					break;
				case 'A':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, term->cursor_pos.x, term->cursor_pos.y - (parameters[0]));
					break;
				case 'B':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, term->cursor_pos.x, term->cursor_pos.y + (parameters[0]));
					break;
				case 'C':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, term->cursor_pos.x + (parameters[0]), term->cursor_pos.y);
					break;
				case 'D':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, term->cursor_pos.x - (parameters[0]), term->cursor_pos.y);
					break;
				case 'G':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, (parameters[0] - 1), term->cursor_pos.y);
					break;
				case 'X':
					if(parameters[0] == 0) parameters[0]++;
					for(uint32_t k = term->cursor_pos.x; k < term->cursor_pos.x + parameters[0]; ++k) {
						term->screen[term->cursor_pos.y][k].utf32 = ' ';
					}
					break;
				case 'b':
					if(parameters[0] == 0) parameters[0]++;
					for(uint32_t k = 0; k < parameters[0]; ++k) {
						term->screen[term->cursor_pos.y][term->cursor_pos.x + k].utf32 = term->last_char;
					}
					vt_setcursor_pos(term, term->cursor_pos.x + parameters[0], term->cursor_pos.y);
					break;
				case 'd':
					parameters[0] = MAX(1, parameters[0]);
					vt_setcursor_pos(term, term->cursor_pos.x, parameters[0] - 1);
					break;
				case 'm':
					vt_set_sgr(term, parameters, param_count);
					break;
				case 'r':
					if(parameters[0] == 0) {
						term->top = 0;
					} else {
						term->top = parameters[0] - 1;
					}

					if(parameters[1] == 0) {
						term->bottom = term->max_rows-1;
					} else {
						term->bottom = parameters[1]-1;
					}
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
	log_debug("Unknown CSI(%s):\n\tPrivate: %c(%x)\n\tIntermediate: %c(%x)\n\tMode: %c(%x)\n\tParameters: [", csi, private, private, intermediate, intermediate, mode, mode);
	for(uint32_t p = 0; p < param_count; ++p) {
		log_printf_raw(TERM_LOG_LEVEL_DEBUG, " %d,", parameters[p]);
	}
	log_printf_raw(TERM_LOG_LEVEL_DEBUG, "]\n");
}

void vt52_escape_process(vt_ctx_t *vt, uint8_t escape) {
	uint8_t posx = 0;
	uint8_t posy = 0;


	switch(escape) {
		case '<':
			vt->term_mode &= ~(TERM_MODE_VT52);
			break;
		case '=':
			vt->term_mode |= TERM_MODE_APP_KEYPAD;
			break;
		case '>':
			vt->term_mode &= ~(TERM_MODE_APP_KEYPAD);
			break;
		case 'A':
			vt_setcursor_pos(vt, vt->cursor_pos.x, vt->cursor_pos.y - 1);
			break;
		case 'B':
			vt_setcursor_pos(vt, vt->cursor_pos.x, vt->cursor_pos.y + 1);
			break;
		case 'C':
			vt_setcursor_pos(vt, vt->cursor_pos.x + 1, vt->cursor_pos.y);
			break;
		case 'D':
			vt_setcursor_pos(vt, vt->cursor_pos.x - 1, vt->cursor_pos.y);
			break;
		case 'F':
			vt->charset = 1;
			break;
		case 'G':
			vt->charset = 0;
			break;
		case 'H':
			vt_setcursor_pos(vt, 0, 0);
			break;
		case 'I':
			vt->cursor_pos.y--;
			if(vt->cursor_pos.y < 0) {
				for(int32_t i = vt->max_rows - 1; i > 0; i--) {
					memcpy(vt->screen[i], vt->screen[i-1], vt->max_cols * sizeof(vt_cell_t));
				}
				vt->cursor_pos.y = 0;
				for(int32_t i = 0; i < vt->max_cols; i++) {
					vt->screen[0][i].utf32 = ' ';
				}
			}
			break;
		case 'J':
			for(int32_t r = vt->cursor_pos.y; r < vt->max_rows; ++r) {
				for(int32_t c = r == vt->cursor_pos.y ? vt->cursor_pos.x : 0; c < vt->max_cols; ++c) {
					vt->screen[r][c].utf32 = ' ';
				}
			}
			break;
		case 'K':
			for(int32_t x = vt->cursor_pos.x; x < vt->max_cols; ++x) {
				vt->screen[vt->cursor_pos.y][x].utf32 = ' ';
			}
			break;
		case 'Y':
			read(vt->ptmx, &posy, 1);
			read(vt->ptmx, &posx, 1);
			vt_setcursor_pos(vt, posx-32, posy-32);
			break;
		case 'Z':
			write(vt->ptmx, "\x1b/Z", strlen("\x1b/Z"));
			break;
		default:
			log_debug("Unknown VT52 escape sequence \\x1b%c\n", escape);
	}
}
/*
void vt_escape_process(term_ctx_t *term) {
	char escape = 0;

	if(term->mode & TERM_MODE_VT52) {
		vt52_escape_process(term);
		return;
	}

	read(term->ptmx, &escape, 1);
	log_debug("\\x1b%c\n", escape);
	switch(escape) {
		case '[':
			vt_csi_handle(term);
			break;
		case ']':
		case 'P':
		case 'k':
			vt_strescape_handle(term, escape);
			break;
		case '=':
			term->mode |= TERM_MODE_APP_KEYPAD;
			break;
		case '>':
			term->mode &= ~(TERM_MODE_APP_KEYPAD);
			break;
		case 'E':
			vt_setcursor_pos(term, 0, term->row+1);
			break;
		case 'D':
			term->row++;
			if(term->row >= term->max_rows) {
				for(int32_t i = 1; i < term->max_rows; i++) {
					memcpy(term->screen[i-1], term->screen[i], term->max_cols * sizeof(term_cell_t));
				}
				term->row = term->max_rows - 1;
				for(int32_t i = 0; i < term->max_cols; i++) {
					term->screen[term->row][i].utf32 = ' ';
				}
			}
			break;
		case 'M':
			term->row--;
			if(term->row < 0) {
				for(int32_t i = term->max_rows - 1; i > 0; i--) {
					memcpy(term->screen[i], term->screen[i-1], term->max_cols * sizeof(term_cell_t));
				}
				term->row = 0;
				for(int32_t i = 0; i < term->max_cols; i++) {
					term->screen[0][i].utf32 = ' ';
				}
			}
		break;
		case '#':
			read(term->ptmx, &escape, 1);
			if(escape == '8') {
				for(int32_t y = 0; y < term->max_rows; y++) {
					for(int32_t x = 0; x < term->max_cols; x++) {
						term->screen[y][x].utf32 = 'E';
					}
				}
			} else {
				log_debug("\\x1b#%c\n", escape);
			}
			break;
		case '%':
			read(term->ptmx, &escape, 1);
			log_debug("\\x1b%%%c\n", escape);
			break;
		case '(':
			read(term->ptmx, &escape, 1);
			if(escape == '0') {
				term->graphics_set = true;
			} else if (escape == 'B') {
				term->graphics_set = false;
			} else {
				term->graphics_set = false;
				log_debug("Unknown charset: \\x1b(%c\n returning to regular charset", escape);
			}
			break;
		default:
			log_debug("Unknown Escape format \\x1b%c\n", escape);
			break;
	}
	return;
}
*/



void vt_scroll(vt_ctx_t *term) {
	if(term->cursor_pos.y >= term->bottom) {
		for(int32_t i = term->top+1; i <= term->bottom; i++) {
			memcpy(term->screen[i-1], term->screen[i], term->max_cols * sizeof(vt_cell_t));
		}
		term->cursor_pos.y = term->bottom;
		for(int32_t i = 0; i < term->max_cols; i++) {
			term->screen[term->cursor_pos.y][i].utf32 = ' ';
		}
		term->cursor_pos.x = 0;
	} else {
		term->cursor_pos.y++;
	}
	term->flags &= ~TERM_FLAGS_NEED_WRAP;
}

void vt_handle_ctrl_code(vt_ctx_t *term, uint32_t code) {
	switch(code) {
		case '\t':
			for(int32_t i = 0; i < 8 - (term->cursor_pos.x % 8); ++i) {
				if(term->cursor_pos.x + i >= term->max_cols) {
					break;
				}
				term->screen[term->cursor_pos.y][term->cursor_pos.x + i].utf32 = ' ';
			}
			term->cursor_pos.x += 8 - (term->cursor_pos.x % 8);
			if(term->cursor_pos.x >= term->max_cols) term->cursor_pos.x = term->max_cols - 1;
			break;
		case '\a':
			break;
		case '\b':
			vt_setcursor_pos(term, term->cursor_pos.x - 1, term->cursor_pos.y);
			break;
		case '\r':
			vt_setcursor_pos(term, 0, term->cursor_pos.y);
			break;
		case '\f':
		case '\v':
		case '\n':
			vt_scroll(term);
			break;
		case 0x1b:
			term->state = TERM_STATE_ESC_START;
			memset(term->csi_buffer, 0, sizeof(term->csi_buffer));
			term->csi_len = 0;
			break;
		default:
			log_debug("Unhandled Control Code: %x\n", code);
			return;
	}
}

void vt_free_screen(vt_cell_t **scr, int32_t rows) {
	for(int32_t r = 0; r < rows; r++) {
		free(scr[r]);
	}
	free(scr);
}

vt_cell_t **vt_allocate_screen(int32_t rows, int32_t cols) {
	vt_cell_t **new = malloc(rows * sizeof(vt_cell_t*));
	if(new == NULL) return NULL;

	for(int32_t r = 0; r < rows; r++) {
		new[r] = malloc(cols * sizeof(vt_cell_t));
		if(new[r] == NULL) {
			vt_free_screen(new, r);
			return NULL;
		}
	}

	return new;
}

int vt_getpty(int *parent, int *child) {
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

static void vt_child_exec_shell(const struct passwd *pw, char *shell, int fd) {
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

int vt_forkshell(int parent, int child, pid_t *pout) {
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
		log_error("Neither $SHELL or pw->pw_shell have a shell set\n");
		return -1;
	}
	pid = fork();

	if(pid < 0) {
		log_error("error fork: %s\n", strerror(errno));
		return -1;
	} else if(pid == 0) {
		close(parent);
		vt_child_exec_shell(pw, shell, child);
	}
	if(pout) *pout = pid;

	close(child);
	return 0;
}

void vt_deinit(vt_ctx_t *vt) {
	vt_free_screen(vt->primary, vt->max_rows);
	vt_free_screen(vt->alt, vt->max_rows);
	
	close(vt->ptmx);
	free(vt);
}

vt_ctx_t *vt_init(uint32_t fg, uint32_t bg) {
	int parent, child;
	vt_ctx_t *vt = calloc(1, sizeof(vt_ctx_t));
	if(!vt) {
		log_error("Failed to allocate VT\n");
		return NULL;
	}

	vt->max_cols = 80;
	vt->max_rows = 25;
	vt->bottom = 24;

	vt->def_bg = bg;
	vt->def_fg = fg;
	vt->fg = fg;
	vt->bg = bg;

	vt->term_mode |= TERM_MODE_WRAPAROUND | TERM_MODE_SHOW_CURSOR;

	vt->primary = vt_allocate_screen(vt->max_rows, vt->max_cols);
	vt->alt = vt_allocate_screen(vt->max_rows, vt->max_cols);
	vt->screen = vt->primary;
	vt->cursor.bg = bg;
	vt->cursor.fg = fg;
	vt_setcursor_shape(vt, 2);
	vt_fallback_color_table(vt);
	vt_clear_screen(vt);

	struct winsize wsz = { vt->max_rows, vt->max_cols, 800, 600 };
	ioctl(vt->ptmx, TIOCSWINSZ, &wsz);

	if(vt_getpty(&parent, &child) == -1) {
		log_error("getpty failed: %s\n", strerror(errno));
		free(vt);
		return NULL;
	}

	if(vt_forkshell(parent, child, &vt->child) == -1) {
		log_error("forkshell failed: %s\n", strerror(errno));
		free(vt);
		close(parent);
		return NULL;
	}
	vt->ptmx = parent;
	return vt;
}
