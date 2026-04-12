#include <stdint.h>
#define _XOPEN_SOURCE 600

#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xkb.h>
#include <xcb/shm.h>


#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <term/display.h>

typedef struct xid_free_list xid_free_list_t;

struct xid_free_list {
	uint32_t id;
	xid_free_list_t *next;
};

typedef struct x11_term_display {
	term_display_t base;
	xcb_connection_t *connection;
	const xcb_setup_t *setup;
	xcb_screen_t *screen;
	xid_free_list_t *free_list;

	struct xkb_context *ctx;
	struct xkb_keymap *keymap;
	struct xkb_state *state;

	uint8_t shm_major;
	uint8_t shm_event;
	uint8_t shm_error;
	uint8_t xkb_event;
	uint8_t xkb_error;

	xcb_intern_atom_reply_t *close;

	xcb_window_t window;
	xcb_gcontext_t gc;
} xcb_term_display_t;

static void xid_free_list_insert(xid_free_list_t **head, xid_free_list_t *new) {
	new->next = *head;
	*head = new;
}

static xid_free_list_t *xid_free_list_pop(xid_free_list_t **head) {
	xid_free_list_t *node = *head;
	if(node == NULL) return NULL;

	*head = node->next;
	return node;
}

static uint32_t get_xid(xcb_term_display_t *xcb) {
	xid_free_list_t *node = xid_free_list_pop(&xcb->free_list);
	uint32_t id = 0;

	if(node) {
		id = node->id;
		free(node);
	} else {
		id = xcb_generate_id(xcb->connection);
	}

	return id;
}

static int xid_free(xcb_term_display_t *xcb, uint32_t id) {
	xid_free_list_t *new = calloc(1, sizeof(xcb_term_display_t));
	if(!new) return -1;

	new->id = id;
	xid_free_list_insert(&xcb->free_list, new);
	return 0;
}

static int term_x11_attach_shm(term_display_t *dpy, int fd, uint32_t width, uint32_t height, uint32_t stride, uint32_t size, uint32_t offset, uint32_t format) {
	xcb_term_display_t *xcb = (xcb_term_display_t*)dpy;
	int dupfd = dup(fd);/*X closes the FD So dup it*/

	xcb_shm_seg_t shmseg = get_xid(xcb);
	xcb_shm_attach_fd(xcb->connection, shmseg, dupfd, 0);

	xcb_pixmap_t pixmap = get_xid(xcb);
	xcb_shm_create_pixmap(xcb->connection, pixmap, xcb->window, width, height, xcb->screen->root_depth, shmseg, offset);

	xcb_shm_detach(xcb->connection, shmseg);
	xid_free(xcb, shmseg);

	xcb_copy_area(xcb->connection, pixmap, xcb->window, xcb->gc, 0, 0, 0, 0, width, height);
	xcb_free_pixmap(xcb->connection, pixmap);
	xid_free(xcb, pixmap);
	xcb_flush(xcb->connection);

	return 0;
}

static void x11_handle_xkb_event(xcb_term_display_t *xcb, xcb_generic_event_t *ev) {
	xcb_xkb_state_notify_event_t *xkb_ev = (xcb_xkb_state_notify_event_t*)ev;

	switch(xkb_ev->xkbType) {
		case XCB_XKB_STATE_NOTIFY:
			xkb_state_update_mask(xcb->state, xkb_ev->baseMods, xkb_ev->latchedMods, xkb_ev->lockedMods, xkb_ev->baseGroup, xkb_ev->latchedGroup, xkb_ev->lockedGroup);
			break;
		default:
			printf("Unhandled XKB Event: %d\n", xkb_ev->xkbType);
			break;
	}
}

static void x11_handle_core_event(xcb_term_display_t *xcb, xcb_generic_event_t *ev) {
	switch(ev->response_type & ~0x80) {
		case XCB_KEY_PRESS: {
			xcb_key_press_event_t *key = (xcb_key_release_event_t*)ev;
			xcb->base.callbacks.keypress(xcb->base.data, key->detail, 1);
			break;
		}	
		case XCB_KEY_RELEASE: {
			xcb_key_release_event_t *key = (xcb_key_release_event_t*)ev;
			xcb->base.callbacks.keypress(xcb->base.data, key->detail, 0);
			break;
		}
		case XCB_CONFIGURE_NOTIFY: {
			xcb_configure_notify_event_t *configure = (xcb_configure_notify_event_t*)ev;
			xcb->base.callbacks.configure(xcb->base.data, configure->width, configure->height);
			break;
		}
		case XCB_CLIENT_MESSAGE: {
			if(((xcb_client_message_event_t*)ev)->data.data32[0] == xcb->close->atom && xcb->base.callbacks.close) {
				xcb->base.callbacks.close(xcb->base.data);
			}
			break;
		}
		case 0: {
			xcb_generic_error_t *err = (xcb_generic_error_t*)ev;
			printf("X11 Error: %d %d.%d\n", err->error_code, err->major_code, err->minor_code);
			break;
		}
		default:
			__builtin_dump_struct(ev, &printf);
			break;
	}
}

static void term_x11_display_dispatch(term_display_t *dpy) {
	static uint32_t first_call = 1;
	xcb_term_display_t *xcb = (xcb_term_display_t*)dpy;
	xcb_generic_event_t *ev = NULL;
	if(first_call) {
		dpy->callbacks.keymap_change(xcb->base.data, xcb->keymap, xcb->state);
		first_call = 0;
	}

	while((ev = xcb_poll_for_event(xcb->connection))) {
		uint8_t type = ev->response_type & ~0x80;
		if(type == xcb->xkb_event) {
			x11_handle_xkb_event(xcb, ev);
		} else {
			x11_handle_core_event(xcb, ev);
		}
		free(ev);
	}
}

void term_x11_display_deinit(term_display_t *dpy) {
	xcb_term_display_t *xcb = (xcb_term_display_t*)dpy;

	xkb_state_unref(xcb->state);
	xkb_keymap_unref(xcb->keymap);
	xkb_context_unref(xcb->ctx);
	free(xcb->close);

	xcb_disconnect(xcb->connection);

	xid_free_list_t *next;
	for(xid_free_list_t *tmp = xcb->free_list; tmp; tmp = next) {
		next = tmp->next;
		free(tmp);
	} 

	free(xcb);
}

term_display_t *term_x11_display_init(void) {
	const xcb_query_extension_reply_t *reply = NULL;
	int screen_no = 0;
	xcb_term_display_t *xcb = calloc(1, sizeof(xcb_term_display_t));

	xcb->connection = xcb_connect(NULL, &screen_no);

	xcb->setup = xcb_get_setup(xcb->connection);
	xcb_screen_iterator_t iter = xcb_setup_roots_iterator(xcb->setup);
	for(int i = 0; i < screen_no; i++) {
		xcb_screen_next(&iter);
	}
	xcb->screen = iter.data;

	reply = xcb_get_extension_data(xcb->connection, &xcb_shm_id);
	xcb_shm_query_version_cookie_t cookie = xcb_shm_query_version(xcb->connection);
	xcb_shm_query_version_reply_t *version = xcb_shm_query_version_reply(xcb->connection, cookie, NULL);
	if(version->major_version != 1 && version->minor_version < 2) {
		printf("Minimum xcb-shm extension version not met. Want 1.2 have %d.%d\n", version->major_version, version->minor_version);
		return NULL;
	}
	free(version);

	uint32_t events = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE;
	xcb->window = xcb_generate_id(xcb->connection);
	xcb_create_window(xcb->connection, xcb->screen->root_depth, xcb->window, xcb->screen->root, 0, 0, 640, 480, 1, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &events);

	xcb_map_window(xcb->connection, xcb->window);
	xcb->gc = xcb_generate_id(xcb->connection);
	xcb_create_gc(xcb->connection, xcb->gc, xcb->window, 0, NULL);
	xcb_flush(xcb->connection);
	xkb_x11_setup_xkb_extension(xcb->connection, XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION, 0, NULL, NULL, &xcb->xkb_event, &xcb->xkb_error);

	xcb->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	xcb->keymap = xkb_x11_keymap_new_from_device(xcb->ctx, xcb->connection, xkb_x11_get_core_keyboard_device_id(xcb->connection), XKB_KEYMAP_COMPILE_NO_FLAGS);
	xcb->state = xkb_x11_state_new_from_device(xcb->keymap, xcb->connection, xkb_x11_get_core_keyboard_device_id(xcb->connection));

	uint16_t xkb_events = XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY | XCB_XKB_EVENT_TYPE_STATE_NOTIFY;
	xcb_xkb_select_events_aux(xcb->connection, XCB_XKB_ID_USE_CORE_KBD, xkb_events, 0, xkb_events, 0, 0, NULL);

	xcb_intern_atom_cookie_t protocol_cookie = xcb_intern_atom_unchecked(xcb->connection, 1, 12, "WM_PROTOCOLS");
	xcb_intern_atom_reply_t *protocol_reply = xcb_intern_atom_reply(xcb->connection, protocol_cookie, NULL);
	xcb_intern_atom_cookie_t close_cookie = xcb_intern_atom_unchecked(xcb->connection, 1, 16, "WM_DELETE_WINDOW");
	xcb->close = xcb_intern_atom_reply(xcb->connection, close_cookie, NULL);
	xcb_change_property(xcb->connection, XCB_PROP_MODE_REPLACE, xcb->window, protocol_reply->atom, 4, 32, 1, &xcb->close->atom);
	free(protocol_reply);

	xcb->base.attach_shm = term_x11_attach_shm;
	xcb->base.dispatch = term_x11_display_dispatch;
	xcb->base.deinit = term_x11_display_deinit;
	return &xcb->base;
}
