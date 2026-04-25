#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/xkb.h>
#include <xcb/shm.h>
#include <xcb/present.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <soda-term/log.h>
#include <soda-term/display.h>

#define UNUSED(x) (void)x

#define X11_ATOM_SELECTION_NAME "CLIPBOARD"
#define X11_ATOM_TARGET_NAME "UTF8_STRING"
#define X11_ATOM_INCR_NAME "INCR"
#define X11_ATOM_PROPERTY_NAME "PROJECTTERM"
#define X11_ATOM_WM_PROTOCOLS_NAME "WM_PROTOCOLS"
#define X11_ATOM_WM_DELETE_WINDOW "WM_DELETE_WINDOW"

typedef struct xid_free_list xid_free_list_t;

struct xid_free_list {
	uint32_t id;
	xid_free_list_t *next;
};

typedef struct soda_buffer_list soda_buffer_list_t;

struct soda_buffer_list {
	uint32_t pixmap_id;
	soda_shm_buffer_t *buffer;
	soda_buffer_list_t *next;
};

typedef struct x11_soda_display {
	soda_display_t base;
	xcb_connection_t *connection;
	const xcb_setup_t *setup;
	xcb_screen_t *screen;
	xid_free_list_t *free_list;
	soda_buffer_list_t *buffer_list;

	struct xkb_context *ctx;
	struct xkb_keymap *keymap;
	struct xkb_state *state;

	uint8_t shm_major;
	uint8_t shm_event;
	uint8_t shm_error;

	uint8_t xkb_event;
	uint8_t xkb_error;

	uint8_t present_event;
	uint8_t present_error;
	xcb_present_event_t eid;
	int presenting;
	int redraw_callback;


	xcb_atom_t wm_protocols;
	xcb_atom_t delete_window;
	xcb_atom_t selection;
	xcb_atom_t target;
	xcb_atom_t property;
	xcb_atom_t incr;

	xcb_visualid_t visid;
	xcb_colormap_t colormap;
	uint8_t bpp;

	xcb_window_t window;
	xcb_gcontext_t gc;
	uint64_t last_msc;
} xcb_soda_display_t;

static const char *x11_error_code_to_str(uint8_t code) {
	switch(code) {
		case XCB_REQUEST: return "BadRequest";
		case XCB_VALUE: return "BadValue";
		case XCB_WINDOW: return "BadWindow";
		case XCB_PIXMAP: return "BadPixmap";
		case XCB_ATOM: return "BadAtom";
		case XCB_CURSOR: return "BadCursor";
		case XCB_FONT: return "BadFont";
		case XCB_MATCH: return "BadMatch";
		case XCB_COLORMAP: return "BadColormap";
		case XCB_G_CONTEXT: return "BadGC";
		case XCB_ID_CHOICE: return "BadID";
		case XCB_NAME: return "BadName";
		case XCB_LENGTH: return "BadLength";
		case XCB_IMPLEMENTATION: return "BadImplementation";
		default: return "Unknown";
	}
}

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

static uint32_t get_xid(xcb_soda_display_t *xcb) {
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

static int xid_free(xcb_soda_display_t *xcb, uint32_t id) {
	xid_free_list_t *new = calloc(1, sizeof(xid_free_list_t));
	if(!new) return -1;

	new->id = id;
	xid_free_list_insert(&xcb->free_list, new);
	return 0;
}

static int term_x11_attach_shm(soda_display_t *dpy, soda_shm_buffer_t *buffer) {
	xcb_soda_display_t *xcb = (xcb_soda_display_t*)dpy;
	int dupfd = dup(buffer->fd);/*X closes the FD So dup it*/

	xcb_shm_seg_t shmseg = get_xid(xcb);
	xcb_shm_attach_fd(xcb->connection, shmseg, dupfd, 0);

	xcb_pixmap_t pixmap = get_xid(xcb);
	xcb_shm_create_pixmap(xcb->connection, pixmap, xcb->window, buffer->width, buffer->height, xcb->bpp, shmseg, 0);

	xcb_shm_detach(xcb->connection, shmseg);
	xid_free(xcb, shmseg);

	xcb_present_pixmap(xcb->connection, xcb->window, pixmap, 0, 0, 0, 0, 0, 0, 0, 0, XCB_PRESENT_OPTION_COPY, 0, 0, 0, 0, NULL);

	buffer->in_use = true;
	xcb->presenting = 1;

	soda_buffer_list_t *new = calloc(1, sizeof(soda_buffer_list_t));
	new->pixmap_id = pixmap;
	new->buffer = buffer;
	if(xcb->buffer_list == NULL) {
		xcb->buffer_list = new;
	} else {
		new->next = xcb->buffer_list;
		xcb->buffer_list = new;
	}

	xcb_flush(xcb->connection);
	return 0;
}

static int term_x11_get_fd(soda_display_t *dpy, int **fds) {
	xcb_soda_display_t *xcb = (xcb_soda_display_t*)dpy;
	if(!fds) return -1;

	*fds = calloc(1, sizeof(int));
	if(*fds == NULL) {
		return -1;
	}

	(*fds)[0] = xcb_get_file_descriptor(xcb->connection);

	return 1;
}

static void x11_handle_xkb_event(xcb_soda_display_t *xcb, xcb_generic_event_t *ev) {
	xcb_xkb_state_notify_event_t *xkb_ev = (xcb_xkb_state_notify_event_t*)ev;

	switch(xkb_ev->xkbType) {
		case XCB_XKB_STATE_NOTIFY:
			xkb_state_update_mask(xcb->state, xkb_ev->baseMods, xkb_ev->latchedMods, xkb_ev->lockedMods, xkb_ev->baseGroup, xkb_ev->latchedGroup, xkb_ev->lockedGroup);
			break;
		default:
			log_debug("Unhandled XKB Event: %d\n", xkb_ev->xkbType);
			break;
	}
}

soda_buffer_list_t *buffer_remove_by_pixmap_id(soda_buffer_list_t **h, xcb_pixmap_t pixmap) {
	soda_buffer_list_t *tmp = *h;
	if(tmp == NULL) return NULL;
	if(tmp->pixmap_id == pixmap) {
		*h = tmp->next;
		return tmp;
	}

	soda_buffer_list_t *prev = tmp;
	tmp = tmp->next;
	for(; tmp; tmp = tmp->next) {
		if(tmp->pixmap_id) {
			prev->next = tmp->next;
			return tmp;
		}
		prev = tmp;
	}

	return NULL;
}

static void x11_handle_present_idle_notify(xcb_soda_display_t *xcb, xcb_ge_generic_event_t *ev) {
	xcb_present_idle_notify_event_t *idle = (xcb_present_idle_notify_event_t*)ev;
	soda_buffer_list_t *tmp;

	tmp = buffer_remove_by_pixmap_id(&xcb->buffer_list, idle->pixmap);
	if(tmp == NULL) {
		log_debug("Pixmap %d has no buffer in the pending buffer list\n", idle->pixmap);
		exit(1);
	}
	tmp->buffer->buffer_freed(tmp->buffer);
	free(tmp);

	xcb_free_pixmap(xcb->connection, idle->pixmap);
	xcb_flush(xcb->connection);
	xid_free(xcb, idle->pixmap);
}



static void x11_handle_xcb_present_event(xcb_soda_display_t *xcb, xcb_ge_generic_event_t *ev) {
	xcb_present_generic_event_t *gev = (xcb_present_generic_event_t*)ev;

	switch(gev->evtype) {
		case XCB_PRESENT_EVENT_COMPLETE_NOTIFY: {
			xcb->presenting = 0;
			xcb->last_msc = ((xcb_present_complete_notify_event_t*)gev)->msc;
			if(xcb->redraw_callback && xcb->base.callbacks.redraw) {
				xcb->redraw_callback = 0;
				xcb->presenting = 1;
				xcb->base.callbacks.redraw(xcb->base.data);
			}
			break;
																						}
		case XCB_PRESENT_EVENT_IDLE_NOTIFY:
			x11_handle_present_idle_notify(xcb, ev);
			break;
	}
}

static void x11_handle_keypress(xcb_soda_display_t *xcb, xcb_generic_event_t *ev) {
	xcb_key_press_event_t *key = (xcb_key_release_event_t*)ev;
	xcb->base.callbacks.keypress(xcb->base.data, key->detail, 1);
}

static void x11_handle_keyrelease(xcb_soda_display_t *xcb, xcb_generic_event_t *ev) {
	xcb_key_release_event_t *key = (xcb_key_release_event_t*)ev;
	xcb->base.callbacks.keypress(xcb->base.data, key->detail, 0);
}

static void x11_handle_configure_notify(xcb_soda_display_t *xcb, xcb_generic_event_t *ev) {
	xcb_configure_notify_event_t *configure = (xcb_configure_notify_event_t*)ev;
	xcb->base.callbacks.configure(xcb->base.data, configure->width, configure->height);
}

static void x11_handle_client_message(xcb_soda_display_t *xcb, xcb_generic_event_t *ev) {
	xcb_client_message_event_t *msg = (xcb_client_message_event_t*)ev;
	if(msg->data.data32[0] == xcb->delete_window && xcb->base.callbacks.close) {
		xcb->base.callbacks.close(xcb->base.data);
	}
}

static void x11_handle_error(xcb_soda_display_t *xcb, xcb_generic_error_t *err) {
	xcb_value_error_t *verr = (xcb_value_error_t*)err;
	log_error("%s(%u) Error:\n\tOpcode: %u.%u\n\tBad Value/ID: %u\n", 
				 x11_error_code_to_str(verr->error_code), verr->error_code,
				 verr->major_opcode, verr->minor_opcode, verr->bad_value);
	UNUSED(xcb);
}

static void x11_handle_selection_notify(xcb_soda_display_t *xcb, xcb_generic_event_t *ev) {
	xcb_selection_notify_event_t *selection = (xcb_selection_notify_event_t*)ev;
	xcb_generic_error_t *err = NULL;

	if(selection->property == XCB_NONE) {
		log_debug("ignoring non UTF8 paste\n");
		return;
	}

	xcb_get_property_cookie_t cookie = xcb_get_property(xcb->connection, false, xcb->window, xcb->property, xcb->target, 0, 0);
	xcb_get_property_reply_t *reply = xcb_get_property_reply(xcb->connection, cookie, &err);
	if(err) {
		log_error("xcb_get_property_reply error: %d\n", err->error_code);
		free(err);
		return;
	}

	if(reply->type == xcb->incr) {
		log_error("data to large and INCR not implemented\n");
		return;
	}
	if(!reply) {
		log_error("allocation failed\n");
		return;
	}

	uint32_t total_sz = reply->bytes_after;
	free(reply);
	if(total_sz == 0) {
		return;
	}

	char *str = calloc(1, total_sz + 1);
	if(str == NULL) {
		log_error("allocation failed\n");
		return;
	}

	cookie = xcb_get_property(xcb->connection, false, xcb->window, xcb->property, xcb->target, 0, total_sz / 4 + 1);
	reply = xcb_get_property_reply(xcb->connection, cookie, &err);
	if(err) {
		log_error("xcb_get_property_reply error: %d\n", err->error_code);
		free(str);
		free(err);
		return;
	}

	if(!reply) {
		log_error("allocation failed\n");
		free(str);
		return;
	}

	memcpy(str, xcb_get_property_value(reply), xcb_get_property_value_length(reply));
	free(reply);
	if(xcb->base.callbacks.clipboard_str_callback) {
		xcb->base.callbacks.clipboard_str_callback(xcb->base.data, str);
	}

	xcb_delete_property(xcb->connection, xcb->window, xcb->property);
	xcb_flush(xcb->connection);
	free(str);
}

static void x11_handle_motion(xcb_soda_display_t *xcb, xcb_generic_event_t *ev) {
	xcb_motion_notify_event_t *motion = (xcb_motion_notify_event_t*)ev;

	if(xcb->base.callbacks.pointer_motion) {
		xcb->base.callbacks.pointer_motion(xcb->base.data, motion->event_x, motion->event_y);
	}
}

static void x11_handle_button(xcb_soda_display_t *xcb, xcb_generic_event_t *ev) {
	xcb_button_press_event_t *btn = (xcb_button_press_event_t*)ev;
	uint32_t pressed = 0;
	if(btn->response_type == XCB_BUTTON_PRESS) pressed = 1;

	if(xcb->base.callbacks.pointer_button) {
		xcb->base.callbacks.pointer_button(xcb->base.data, btn->detail, pressed);
	}
}

static void x11_handle_focus_change(xcb_soda_display_t *xcb, xcb_generic_event_t *ev) {
	uint32_t focus = 0;
	if(ev->response_type == XCB_FOCUS_IN) {
		focus = 1;
	}

	if(xcb->base.callbacks.pointer_focus) {
		xcb->base.callbacks.pointer_focus(xcb->base.data, focus);
	}

}

static void x11_handle_core_event(xcb_soda_display_t *xcb, xcb_generic_event_t *ev) {
	switch(ev->response_type & ~0x80) {
		case XCB_KEY_PRESS:
			x11_handle_keypress(xcb, ev);
			break;
		case XCB_KEY_RELEASE:
			x11_handle_keyrelease(xcb, ev);
			break;
		case XCB_CONFIGURE_NOTIFY:
			x11_handle_configure_notify(xcb, ev);
			break;
		case XCB_CLIENT_MESSAGE:
			x11_handle_client_message(xcb, ev);
			break;
		case XCB_SELECTION_NOTIFY:
			x11_handle_selection_notify(xcb, ev);
			break;
		case XCB_FOCUS_IN:
		case XCB_FOCUS_OUT:
			x11_handle_focus_change(xcb, ev);
			break;
		case XCB_BUTTON_PRESS:
		case XCB_BUTTON_RELEASE:
			x11_handle_button(xcb, ev);
			break;
		case XCB_MOTION_NOTIFY:
			x11_handle_motion(xcb, ev);
			break;
		case 0:
			x11_handle_error(xcb, (xcb_generic_error_t*)ev);
			break;
		default:
			break;
	}
}

static void term_x11_request_clipboard_text(soda_display_t *dpy) {
	xcb_soda_display_t *xcb = (xcb_soda_display_t*)dpy;

	xcb_convert_selection(xcb->connection, xcb->window, xcb->selection, xcb->target, xcb->property, XCB_CURRENT_TIME);
	xcb_flush(xcb->connection);
}

static void term_x11_request_redraw_callback(soda_display_t *dpy) {
	xcb_soda_display_t *xcb = (xcb_soda_display_t*)dpy;
	xcb->redraw_callback = 1;
	if(xcb->presenting == 0 && xcb->base.callbacks.redraw) {
		xcb->presenting = 1;
		xcb->redraw_callback = 0;
		xcb->base.callbacks.redraw(xcb->base.data);
	}
}

static void term_x11_display_dispatch(soda_display_t *dpy) {
	static uint32_t first_call = 1;
	xcb_soda_display_t *xcb = (xcb_soda_display_t*)dpy;
	xcb_generic_event_t *ev = NULL;
	if(first_call) {
		dpy->callbacks.keymap_change(xcb->base.data, xcb->keymap, xcb->state);
		first_call = 0;
	}
	xcb_flush(xcb->connection);

	while((ev = xcb_poll_for_event(xcb->connection))) {
		uint8_t type = ev->response_type & ~0x80;
		if(type == xcb->xkb_event) {
			x11_handle_xkb_event(xcb, ev);
		} else if(type == XCB_GE_GENERIC) {
			xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t*)ev;
			if(ge->extension == xcb->present_event) {
				x11_handle_xcb_present_event(xcb, ge);
			}
		} else {
			x11_handle_core_event(xcb, ev);
		}
		free(ev);
	}
	xcb_flush(xcb->connection);
}

void term_x11_display_deinit(soda_display_t *dpy) {
	xcb_soda_display_t *xcb = (xcb_soda_display_t*)dpy;

	while(xcb->buffer_list) {
		xcb->base.dispatch(dpy);
	}

	xkb_state_unref(xcb->state);
	xkb_keymap_unref(xcb->keymap);
	xkb_context_unref(xcb->ctx);

	xcb_disconnect(xcb->connection);

	xid_free_list_t *next;
	for(xid_free_list_t *tmp = xcb->free_list; tmp; tmp = next) {
		next = tmp->next;
		free(tmp);
	}

	free(xcb);
}

int x11_match_visual(xcb_screen_t *screen, uint8_t bpp, uint8_t class, uint32_t *vid) {
	xcb_visualtype_iterator_t visual_iter;
	xcb_depth_iterator_t depth_iter = xcb_screen_allowed_depths_iterator(screen);
	xcb_visualtype_t *visual = NULL;
	xcb_depth_t *depth = NULL;

	int found = 0;
	for(; depth_iter.rem; xcb_depth_next(&depth_iter)) {
		if(depth_iter.data->depth == bpp) {
			found = 1;
			break;
		}
	}
	depth = depth_iter.data;
	if(!found || depth_iter.data->visuals_len == 0) return -1;

	found = 0;
	visual_iter = xcb_depth_visuals_iterator(depth);
	for(; visual_iter.rem; xcb_visualtype_next(&visual_iter)) {
		if(visual_iter.data->_class == class) {
			found = 1;
			break;
		}
	}
	visual = visual_iter.data;
	if(!found) return -1;
	*vid = visual->visual_id;

	return 0;
}

static int x11_check_shm_version(xcb_connection_t *xcb) {
	xcb_shm_query_version_cookie_t cookie;
	xcb_shm_query_version_reply_t *version;
	const xcb_query_extension_reply_t *ext;
	xcb_generic_error_t *err = NULL;

	ext = xcb_get_extension_data(xcb, &xcb_shm_id);
	if(!ext || ext->present == 0) return -1;

	cookie = xcb_shm_query_version(xcb);
	version = xcb_shm_query_version_reply(xcb, cookie, &err);
	if(err) {
		log_error("xcb_shm_query_version error: %d\n", err->error_code);
		free(err);
		return -1;
	}

	if(!version) return -1;
	if(version->major_version != 1 || version->minor_version < 2) {
		log_error("xcb-shm incompatible version: want 1.2 have %d.%d", version->major_version, version->minor_version);
		free(version);
		return -1;
	}
	free(version);
	return 0;
}

static int x11_get_atom(xcb_connection_t *c, uint8_t if_exists, const char *name, xcb_atom_t *a) {
	xcb_intern_atom_cookie_t cookie;
	xcb_intern_atom_reply_t *reply = NULL;
	xcb_generic_error_t *error = NULL;
	if(!a) return -1;

	cookie = xcb_intern_atom(c, if_exists, strlen(name), name);
	reply = xcb_intern_atom_reply(c, cookie, &error);
	if(error) {
		log_error("xcb_intern_atom_reply error: %d\n", error->error_code);
		free(error);
		return -1;
	}

	if(!reply) {
		log_error("xcb_intern_atom_reply allocation error\n");
		return -1;
	}

	*a = reply->atom;
	free(reply);
	return 0;
}

int soda_x11_init_xkb(xcb_connection_t *c, uint16_t major, uint16_t minor, uint8_t *base_event, uint8_t *base_error) {
	const xcb_query_extension_reply_t *reply;
	xcb_generic_error_t *error = NULL;
	xcb_xkb_use_extension_cookie_t cookie;
	xcb_xkb_use_extension_reply_t *use_reply = NULL;

	reply = xcb_get_extension_data(c, &xcb_xkb_id);
	if(!reply) {
		log_error("Failed to query xcb_xkb extensions\n");
		return -1;
	}

	if(reply->present == 0) {
		log_error("xcb_xkb extension error: Extension not present\n");
		return -1;
	}

	cookie = xcb_xkb_use_extension(c, 1, 0);
	use_reply = xcb_xkb_use_extension_reply(c, cookie, &error);
	if(error) {
		log_error("xcb_xkb extension error: %s\n", x11_error_code_to_str(error->error_code));
		free(error);
		return -1;
	}

	if(use_reply->supported == 0) {
		log_error("xcb_xkb extension error: Version %d.%d not supported Server version %d.%d\n", major, minor, use_reply->serverMajor, use_reply->serverMinor);
		free(use_reply);
		return -1;
	}

	if(base_event) *base_event = reply->first_event;
	if(base_error) *base_error = reply->first_error;
	free(use_reply);
	return 0;	
}

soda_display_t *soda_x11_display_init(void) {
	xcb_soda_display_t *xcb = calloc(1, sizeof(xcb_soda_display_t));
	xcb_screen_iterator_t iter;
	xcb_void_cookie_t cookie;
	xcb_generic_error_t *err = NULL;
	if(!xcb) return NULL;

	int screen_no = 0;

	xcb->connection = xcb_connect(NULL, &screen_no);
	if(xcb_connection_has_error(xcb->connection)) {
		goto err_xcb_disconnect;
	}

	xcb->setup = xcb_get_setup(xcb->connection);
	iter = xcb_setup_roots_iterator(xcb->setup);
	for(int i = 0; i < screen_no; i++) {
		xcb_screen_next(&iter);
	}
	xcb->screen = iter.data;

	if(x11_check_shm_version(xcb->connection) == -1) {
		goto err_xcb_disconnect;
	}

	xcb->bpp = 32;
	if(x11_match_visual(xcb->screen, 32, XCB_VISUAL_CLASS_TRUE_COLOR, &xcb->visid) == -1) {
		log_warn("Failed to find 32bit color depth trying 24bit\n");
		xcb->bpp = 24;
		if(xcb->screen->root_depth == 24) {
			xcb->visid = xcb->screen->root_visual;
		} else if(x11_match_visual(xcb->screen, 24, XCB_VISUAL_CLASS_TRUE_COLOR, &xcb->visid) == -1) {
			log_error("No 32 or 24bit color depth.\n");
			goto err_xcb_disconnect;
		}
	}

	if(xcb->visid == xcb->screen->root_visual) {
		xcb->colormap = xcb->screen->default_colormap;
	} else {
		xcb->colormap = xcb_generate_id(xcb->connection);
		xcb_create_colormap(xcb->connection, XCB_COLORMAP_ALLOC_NONE, xcb->colormap, xcb->screen->root, xcb->visid);
	}

	uint32_t winevents = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
											 XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
											 XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
											 XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_FOCUS_CHANGE;
	uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK | XCB_CW_COLORMAP;
	uint32_t values[] = { XCB_PIXMAP_NONE, 0x000000, winevents, xcb->colormap };

	xcb->window = xcb_generate_id(xcb->connection);
	cookie = xcb_create_window_checked(xcb->connection, xcb->bpp, xcb->window,
										xcb->screen->root, 0, 0, 640, 480, 1,
										XCB_WINDOW_CLASS_INPUT_OUTPUT, xcb->visid,
										mask, values);
	err = xcb_request_check(xcb->connection, cookie);
	if(err) {
		log_error("x11: xcb_create_window error %s(%d)\n", x11_error_code_to_str(err->error_code), err->error_code);
		free(err);
		goto err_xcb_disconnect;
	}
	xcb_map_window(xcb->connection, xcb->window);

	xcb->gc = xcb_generate_id(xcb->connection);
	cookie = xcb_create_gc_checked(xcb->connection, xcb->gc, xcb->window, 0, NULL);
	err = xcb_request_check(xcb->connection, cookie);
	if(err) {
		log_error("xcb_create_gc error: %s(%d)\n", x11_error_code_to_str(err->error_code), err->error_code);
		free(err);
		goto err_xcb_disconnect;
	}

	int ret = soda_x11_init_xkb(xcb->connection, 1, 0, &xcb->xkb_event, &xcb->xkb_error);
	if(ret < 0) {
		log_error("xkb_x11_setup_xkb_extension error\n");
		goto err_xcb_disconnect;
	}

	int32_t device_id = xkb_x11_get_core_keyboard_device_id(xcb->connection);
	if(device_id == -1) {
		log_error("xkb_x11_get_core_keyboard_device_id error\n");
		goto err_xcb_disconnect;
	}

	xcb->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	xcb->keymap = xkb_x11_keymap_new_from_device(xcb->ctx, xcb->connection, device_id, XKB_KEYMAP_COMPILE_NO_FLAGS);
	xcb->state = xkb_x11_state_new_from_device(xcb->keymap, xcb->connection, device_id);

	uint16_t xkb_events = XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY |
												XCB_XKB_EVENT_TYPE_MAP_NOTIFY |
												XCB_XKB_EVENT_TYPE_STATE_NOTIFY;

	xcb_xkb_select_events_aux(xcb->connection, XCB_XKB_ID_USE_CORE_KBD, xkb_events, 0, xkb_events, 0, 0, NULL);

	const xcb_query_extension_reply_t *present = xcb_get_extension_data(xcb->connection, &xcb_present_id);
	if(present == NULL || present->present == 0) {
		log_error("xcb_get_extension_data: unable to get present extension\n");
		goto err_xcb_disconnect;
	}
	xcb->present_event = present->major_opcode;
	xcb->eid = xcb_generate_id(xcb->connection);

	cookie = xcb_present_select_input_checked(xcb->connection, xcb->eid, xcb->window, XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY | XCB_PRESENT_EVENT_MASK_CONFIGURE_NOTIFY | XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
	err = xcb_request_check(xcb->connection, cookie);
	if(err) {
		log_error("Failed to select present events\n");
		free(err);
		goto err_xcb_disconnect;
	}

	if(x11_get_atom(xcb->connection, true, X11_ATOM_WM_PROTOCOLS_NAME, &xcb->wm_protocols) == -1) {
		log_error("get_atom %s error\n", X11_ATOM_WM_PROTOCOLS_NAME);
		goto err_xcb_disconnect;
	}

	if(x11_get_atom(xcb->connection, true, X11_ATOM_WM_DELETE_WINDOW, &xcb->delete_window) == -1) {
		log_error("get_atom %s error\n", X11_ATOM_WM_DELETE_WINDOW);
		goto err_xcb_disconnect;
	}

	xcb_change_property(xcb->connection, XCB_PROP_MODE_REPLACE, xcb->window, xcb->wm_protocols, 4, 32, 1, &xcb->delete_window);

	if(x11_get_atom(xcb->connection, false, X11_ATOM_SELECTION_NAME, &xcb->selection) == -1) {
		log_error("x11: get_atom %s error\n", X11_ATOM_SELECTION_NAME);
		goto err_xcb_disconnect;
	}

	if(x11_get_atom(xcb->connection, false, X11_ATOM_TARGET_NAME, &xcb->target) == -1) {
		log_error("x11: get_atom %s error\n", X11_ATOM_TARGET_NAME);
		goto err_xcb_disconnect;
	}

	if(x11_get_atom(xcb->connection, false, X11_ATOM_PROPERTY_NAME, &xcb->property) == -1) {
		log_error("x11: get_atom %s error\n", X11_ATOM_PROPERTY_NAME);
		goto err_xcb_disconnect;
	}

	if(x11_get_atom(xcb->connection, 0, X11_ATOM_INCR_NAME, &xcb->incr) == -1) {
		log_error("x11: get_atom %s error\n", X11_ATOM_INCR_NAME);
		goto err_xcb_disconnect;
	}

	xcb_flush(xcb->connection);
	xcb->base.attach_shm = term_x11_attach_shm;
	xcb->base.dispatch = term_x11_display_dispatch;
	xcb->base.deinit = term_x11_display_deinit;
	xcb->base.display_fds = term_x11_get_fd;
	xcb->base.request_cliboard_text = term_x11_request_clipboard_text;
	xcb->base.request_frame_callback = term_x11_request_redraw_callback;
	return &xcb->base;

err_xcb_disconnect:
	xcb_disconnect(xcb->connection);
	free(xcb);
	return NULL;
}
