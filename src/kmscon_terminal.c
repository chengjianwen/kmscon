/**
 * kmscon - Terminal
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Terminal
 * A terminal gets assigned an input stream and several output objects and then
 * runs a fully functional terminal emulation on it.
 */

#include <errno.h>
#include <inttypes.h>
#include <libtsm.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <nanomsg/nn.h>
#include <nanomsg/pair.h>
#include "conf.h"
#include "eloop.h"
#include "kmscon_conf.h"
#include "kmscon_seat.h"
#include "kmscon_terminal.h"
#include "pty.h"
#include "shl_dlist.h"
#include "shl_array.h"
#include "shl_log.h"
#include "text.h"
#include "uterm_input.h"
#include "uterm_video.h"
#include "kmscon_utf8.h"
#include "kmscon_im.h"
#include "kmscon_pinyin.h"

#define LOG_SUBSYSTEM "terminal"

#ifndef	EVDEV_KEYCODE_OFFSET
#define	EVDEV_KEYCODE_OFFSET	8
#endif

#define	SPY_PORT	7788
struct screen {
	struct shl_dlist list;
	struct kmscon_terminal *term;
	struct uterm_display *disp;
	struct kmscon_text *txt;

	bool swapping;
	bool pending;
};

struct kmscon_terminal {
	unsigned long ref;
	struct ev_eloop *eloop;
	struct uterm_input *input;
	bool opened;
	bool awake;

	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;
	struct kmscon_session *session;

	struct shl_dlist screens;
	unsigned int min_cols;
	unsigned int min_rows;

	struct tsm_screen *console;
	struct tsm_vte *vte;
	struct kmscon_pty *pty;
	struct ev_fd *ptyfd;

	struct kmscon_font_attr font_attr;
	struct kmscon_font *font;
	struct kmscon_font *bold_font;

/*
 *  输入法及输入法状态
 */
	struct im *im;
/*
 *  远程控制
 */
	struct ev_fd *fd;
	int nn_sock;
        int controled;
/*
 * 心跳
 */
	struct ev_timer *putong;
};

struct tsm_cell {
    uint32_t ch;
    struct   tsm_screen_attr attr;
};

static int control_cb (struct tsm_screen *con,
		uint32_t id,
		const uint32_t *ch,
		size_t len,
		unsigned int width,
		unsigned int posx,
		unsigned int posy,
		const struct tsm_screen_attr *attr,
		tsm_age_t age,
		void *data)
{
	int cols = tsm_screen_get_width (con);
	struct tsm_cell *cells = data;
	memcpy (&cells[posx + posy * cols].ch, ch, sizeof (uint32_t) );
	memcpy (&cells[posx + posy * cols].attr, attr, sizeof (struct tsm_screen_attr) );
	return 0;
}

static void control_event (void *data)
{
	struct	kmscon_terminal *term = data;

	void *cells, *msg;
	
	int cols = tsm_screen_get_width (term->console);
        int lines = tsm_screen_get_height (term->console);
        int pos_x = tsm_screen_get_cursor_x (term->console);
        int pos_y = tsm_screen_get_cursor_y (term->console);
	int size = cols * lines * sizeof (struct tsm_cell);
        /*
         * 获取并发送屏幕内容
         */
        msg = nn_allocmsg(100 + size, 0);
        sprintf (msg, "screen_on %d %d %d %d\n\n", cols, lines, pos_x, pos_y);
        cells = strstr((char *)msg, "\n\n") + 2;
	tsm_screen_draw (term->console, control_cb, cells);

	nn_send (term->nn_sock, &msg, NN_MSG, NN_DONTWAIT);
}

int get_uptime()
{
	struct sysinfo  info;
	sysinfo (&info);
	return info.uptime;
}

void putong_callback (struct ev_timer *timer, long long unsigned num, void *data)
{
	struct kmscon_terminal	*term = data;

	void *buf = nn_allocmsg(30, 0);
	sprintf (buf, "putong\n\n%d", get_uptime()); // TODO: 是否可以通过num获取uptime?
	nn_send (term->nn_sock, &buf, NN_MSG, NN_DONTWAIT);
}

void nn_callback (struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_terminal	*term = data;
	void *msg;
	nn_recv (term->nn_sock, &msg, NN_MSG, NN_DONTWAIT);
	if (strncmp (msg, "screen_on", strlen("screen_on")) == 0) {
                term->controled = 1;
                control_event (data);
	} else if (strncmp (msg, "screen_off", strlen("screen_off")) == 0) {
                term->controled = 0;
	} else if (strncmp (msg, "power_off", strlen("power_off")) == 0) {
		system ("poweroff");
	} else if (strncmp (msg, "reboot", strlen("reboot")) == 0) {
		system ("reboot");
	}
	nn_freemsg (msg);
}

static void im_output_callback (const char *u8, size_t len, void *data)
{
	struct kmscon_terminal *term = data;
	kmscon_pty_write(term->pty, u8, len);
}

static void ime_load_callback (struct shl_array *py)
{
	struct tsm_utf8_mach *mach;
	tsm_utf8_mach_new (&mach);

	for (int i = 0; i < PINYIN_SIZE; i++)
	{
		struct im_ime *d = malloc (sizeof(struct im_ime));
		d->pre = pinyin[i][0];
		shl_array_new (&d->cand, sizeof (uint32_t), 0);
		tsm_utf8_mach_reset (mach);
		for (int j = 0; j < strlen(pinyin[i][1]); j++)
		{
			if (tsm_utf8_mach_feed (mach, pinyin[i][1][j]) == TSM_UTF8_ACCEPT)
			{
				uint32_t ch = tsm_utf8_mach_get (mach);
				shl_array_push (d->cand, &ch);
			}
		}
		shl_array_push (py, d);
	}
	tsm_utf8_mach_free (mach);
}

static void do_clear_margins(struct screen *scr)
{
	unsigned int w, h, sw, sh;
	struct uterm_mode *mode;
	int dw, dh;

	mode = uterm_display_get_current(scr->disp);
	if (!mode)
		return;

	sw = uterm_mode_get_width(mode);
	sh = uterm_mode_get_height(mode);
	w = scr->txt->font->attr.width * scr->txt->cols;
	h = scr->txt->font->attr.height * scr->txt->rows;
	dw = sw - w;
	dh = sh - h;

	if (dw > 0)
		uterm_display_fill(scr->disp, 0, 0, 0,
				   w, 0,
				   dw, h);
	if (dh > 0)
		uterm_display_fill(scr->disp, 0, 0, 0,
				   0, h,
				   sw, dh);
}

void im_preedit_draw_callback(struct im *_im, int index, uint32_t id, uint32_t *ch, size_t len, void *data)
{
	struct tsm_screen_attr attr;
	attr.br = 255;
	attr.bg = 255;
	attr.bb = 255;
	attr.fr = 0;
	attr.fg = 0;
	attr.fb = 0;
	attr.bold = 0;
	attr.underline = 0;
	attr.protect = 0;
	attr.blink = 0;
	attr.inverse = 0;
	struct kmscon_text	*txt = data;
	unsigned int width = tsm_ucs4_get_width (*ch) * len;
	kmscon_text_draw (txt, id, ch, len,
			width, index, txt->rows - 1, &attr);
}

void im_candidates_draw_callback(struct im *_im, int index, uint32_t id, uint32_t *ch, size_t len, bool selected, void *data)
{
	struct tsm_screen_attr attr;
	attr.br = 255;
	attr.bg = 255;
	attr.bb = 255;
	attr.fr = 0;
	attr.fg = 0;
	attr.fb = 0;
	attr.bold = 0;
	attr.underline = 0;
	attr.protect = 0;
	attr.blink = 0;
	attr.inverse = selected ? 1 : 0;
	struct kmscon_text	*txt = data;
	unsigned int width = tsm_ucs4_get_width (*ch);

	kmscon_text_draw (txt, id, ch, len,
			width, 10 + index * width, txt->rows - 1, &attr);
	for (int i = 1; i < width; i++)
		kmscon_text_draw (txt, 0, 0, 0,
				0, 10 + index * width + i, txt->rows - 1, &attr);
}

static void do_redraw_screen(struct screen *scr)
{
	int ret;

	if (!scr->term->awake)
		return;

	scr->pending = false;
	do_clear_margins(scr);

	kmscon_text_prepare(scr->txt);
	tsm_screen_draw(scr->term->console, kmscon_text_draw_cb, scr->txt);
	if (im_isactive( scr->term->im))
		im_draw(scr->term->im, im_preedit_draw_callback, im_candidates_draw_callback, scr->txt->cols, scr->txt);
	kmscon_text_render(scr->txt);

	ret = uterm_display_swap(scr->disp, false);
	if (ret) {
		log_warning("cannot swap display %p", scr->disp);
		return;
	}

	scr->swapping = true;
}

static void redraw_screen(struct screen *scr)
{
	if (!scr->term->awake)
		return;

	if (scr->swapping)
		scr->pending = true;
	else
		do_redraw_screen(scr);
}

static void redraw_all(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	if (!term->awake)
		return;

	shl_dlist_for_each(iter, &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
		redraw_screen(scr);
	}
	if (term->controled )
		control_event (term);
}

static void redraw_all_test(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	if (!term->awake)
		return;

	shl_dlist_for_each(iter, &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
		if (uterm_display_is_swapping(scr->disp))
			scr->swapping = true;
		redraw_screen(scr);
	}
}

static void display_event(struct uterm_display *disp,
			  struct uterm_display_event *ev, void *data)
{
	struct screen *scr = data;

	if (ev->action != UTERM_PAGE_FLIP)
		return;

	scr->swapping = false;
	if (scr->pending)
		do_redraw_screen(scr);
}

/*
 * Resize terminal
 * We support multiple monitors per terminal. As some software-rendering
 * backends to not support scaling, we always use the smallest cols/rows that are
 * provided so wider displays will have black margins.
 * This can be extended to support scaling but that would mean we need to check
 * whether the text-renderer backend supports that, first (TODO).
 *
 * If @force is true, then the console/pty are notified even though the size did
 * not changed. If @notify is false, then console/pty are not notified even
 * though the size might have changed. force = true and notify = false doesn't
 * make any sense, though.
 */
static void terminal_resize(struct kmscon_terminal *term,
			    unsigned int cols, unsigned int rows,
			    bool force, bool notify)
{
	bool resize = false;

	if (!term->min_cols || (cols > 0 && cols < term->min_cols)) {
		term->min_cols = cols;
		resize = true;
	}
	if (!term->min_rows || (rows > 0 && rows < term->min_rows)) {
		term->min_rows = rows;
		resize = true;
	}

	if (!notify || (!resize && !force))
		return;
	if (!term->min_cols || !term->min_rows)
		return;

	tsm_screen_resize(term->console, term->min_cols, im_isactive(term->im) ? term->min_rows - 1: term->min_rows);

	kmscon_pty_resize(term->pty, term->min_cols, term->min_rows);
	redraw_all(term);
}

static int font_set(struct kmscon_terminal *term)
{
	int ret;
	struct kmscon_font *font, *bold_font;
	struct shl_dlist *iter;
	struct screen *ent;

	term->font_attr.bold = false;
	ret = kmscon_font_find(&font, &term->font_attr,
			       term->conf->font_engine);
	if (ret)
		return ret;

	term->font_attr.bold = true;
	ret = kmscon_font_find(&bold_font, &term->font_attr,
			       term->conf->font_engine);
	if (ret) {
		log_warning("cannot create bold font: %d", ret);
		bold_font = font;
		kmscon_font_ref(bold_font);
	}

	kmscon_font_unref(term->bold_font);
	kmscon_font_unref(term->font);
	term->font = font;
	term->bold_font = bold_font;

	term->min_cols = 0;
	term->min_rows = 0;
	shl_dlist_for_each(iter, &term->screens) {
		ent = shl_dlist_entry(iter, struct screen, list);

		ret = kmscon_text_set(ent->txt, font, bold_font, ent->disp);
		if (ret)
			log_warning("cannot change text-renderer font: %d",
				    ret);

		terminal_resize(term,
				kmscon_text_get_cols(ent->txt),
				kmscon_text_get_rows(ent->txt),
				false, false);
	}

	terminal_resize(term, 0, 0, true, true);
	return 0;
}

static int add_display(struct kmscon_terminal *term, struct uterm_display *disp)
{
	struct shl_dlist *iter;
	struct screen *scr;
	int ret;
	const char *be;
	bool opengl;

	shl_dlist_for_each(iter, &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
		if (scr->disp == disp)
			return 0;
	}

	scr = malloc(sizeof(*scr));
	if (!scr) {
		log_error("cannot allocate memory for display %p", disp);
		return -ENOMEM;
	}
	memset(scr, 0, sizeof(*scr));
	scr->term = term;
	scr->disp = disp;

	ret = uterm_display_register_cb(scr->disp, display_event, scr);
	if (ret) {
		log_error("cannot register display callback: %d", ret);
		goto err_free;
	}

	ret = uterm_display_use(scr->disp, &opengl);
	if (term->conf->render_engine)
		be = term->conf->render_engine;
	else if (ret >= 0 && opengl)
		be = "gltex";
	else
		be = "bbulk";

	ret = kmscon_text_new(&scr->txt, be);
	if (ret) {
		log_error("cannot create text-renderer");
		goto err_cb;
	}

	ret = kmscon_text_set(scr->txt, term->font, term->bold_font,
			      scr->disp);
	if (ret) {
		log_error("cannot set text-renderer parameters");
		goto err_text;
	}

	terminal_resize(term,
			kmscon_text_get_cols(scr->txt),
			kmscon_text_get_rows(scr->txt),
			false, true);

	shl_dlist_link(&term->screens, &scr->list);

	log_debug("added display %p to terminal %p", disp, term);
	redraw_screen(scr);
	uterm_display_ref(scr->disp);
	return 0;

err_text:
	kmscon_text_unref(scr->txt);
err_cb:
	uterm_display_unregister_cb(scr->disp, display_event, scr);
err_free:
	free(scr);
	return ret;
}

static void free_screen(struct screen *scr, bool update)
{
	struct shl_dlist *iter;
	struct screen *ent;
	struct kmscon_terminal *term = scr->term;

	log_debug("destroying terminal screen %p", scr);
	shl_dlist_unlink(&scr->list);
	kmscon_text_unref(scr->txt);
	uterm_display_unregister_cb(scr->disp, display_event, scr);
	uterm_display_unref(scr->disp);
	free(scr);

	if (!update)
		return;

	term->min_cols = 0;
	term->min_rows = 0;
	shl_dlist_for_each(iter, &term->screens) {
		ent = shl_dlist_entry(iter, struct screen, list);
		terminal_resize(term,
				kmscon_text_get_cols(ent->txt),
				kmscon_text_get_rows(ent->txt),
				false, false);
	}

	terminal_resize(term, 0, 0, true, true);
}

static void rm_display(struct kmscon_terminal *term, struct uterm_display *disp)
{
	struct shl_dlist *iter;
	struct screen *scr;

	shl_dlist_for_each(iter, &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
		if (scr->disp == disp)
			break;
	}

	if (iter == &term->screens)
		return;

	log_debug("removed display %p from terminal %p", disp, term);
	free_screen(scr, true);
}

static void input_event(struct uterm_input *input,
			struct uterm_input_event *ev,
			void *data)
{
	struct kmscon_terminal *term = data;

	if (!term->opened || !term->awake || ev->handled)
		return;

	if (conf_grab_matches(term->conf->grab_scroll_up,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_up(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_scroll_down,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_down(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_page_up,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_page_up(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_page_down,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		tsm_screen_sb_page_down(term->console, 1);
		redraw_all(term);
		ev->handled = true;
		return;
	}
	if (conf_grab_matches(term->conf->grab_zoom_in,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (term->font_attr.points + 1 < term->font_attr.points)
			return;

		++term->font_attr.points;
		if (font_set(term))
			--term->font_attr.points;
		return;
	}
	if (conf_grab_matches(term->conf->grab_zoom_out,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (term->font_attr.points <= 1)
			return;

		--term->font_attr.points;
		if (font_set(term))
			++term->font_attr.points;
		return;
	}

	/* TODO: xkbcommon supports multiple keysyms, but it is currently
	 * unclear how this feature will be used. There is no keymap, which
	 * uses this, yet. */
	if (ev->num_syms > 1)
		return;

	if (conf_grab_matches(term->conf->active_control,
			      ev->mods, ev->num_syms, ev->keysyms)) {
                ev->handled = true;
                if (term->controled)
			term->controled = 0;
                else
			term->controled = 1;
                return;
        }
	if (conf_grab_matches(term->conf->active_cjk_input,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		im_actived (term->im, !im_isactive(term->im));
// 输入法的界面
		if (im_isactive(term->im))
			im_reset (term->im);
		if (im_isactive(term->im))
		{
			tsm_screen_move_down (term->console, 1, true);
			tsm_screen_scroll_down (term->console, 1);
		}
		terminal_resize(term, 0, 0, true, true);
		redraw_all(term);
		return;
	}

	if (im_isactive(term->im))
		im_keyboard (term->im, ev->keycode - EVDEV_KEYCODE_OFFSET, im_output_callback, &ev->handled, term);

	if (ev->handled)
	{
		redraw_all(term);
		return;
	}

	if (tsm_vte_handle_keyboard(term->vte, ev->keysyms[0], ev->ascii,
				    ev->mods, ev->codepoints[0])) {
		tsm_screen_sb_reset(term->console);
		redraw_all(term);
		ev->handled = true;
	}
}

static void rm_all_screens(struct kmscon_terminal *term)
{
	struct shl_dlist *iter;
	struct screen *scr;

	while ((iter = term->screens.next) != &term->screens) {
		scr = shl_dlist_entry(iter, struct screen, list);
		free_screen(scr, false);
	}

	term->min_cols = 0;
	term->min_rows = 0;
}

static int terminal_open(struct kmscon_terminal *term)
{
	int ret;
	unsigned short width, height;

	if (term->opened)
		return -EALREADY;

	tsm_vte_hard_reset(term->vte);
	width = tsm_screen_get_width(term->console);
	height = tsm_screen_get_height(term->console);
	ret = kmscon_pty_open(term->pty, width, height);
	if (ret)
		return ret;

	term->opened = true;
	redraw_all(term);
	return 0;
}

static void terminal_close(struct kmscon_terminal *term)
{
	kmscon_pty_close(term->pty);
	term->opened = false;
}

static void terminal_destroy(struct kmscon_terminal *term)
{
	log_debug("free terminal object %p", term);

	terminal_close(term);
	rm_all_screens(term);
	uterm_input_unregister_cb(term->input, input_event, term);
	ev_eloop_rm_fd(term->ptyfd);
	kmscon_pty_unref(term->pty);
	kmscon_font_unref(term->bold_font);
	kmscon_font_unref(term->font);
        im_destroy (term->im);
	tsm_vte_unref(term->vte);
	tsm_screen_unref(term->console);
	uterm_input_unref(term->input);
	nn_close(term->nn_sock);
	ev_eloop_rm_fd(term->fd);
	ev_eloop_unref(term->eloop);
	im_destroy (term->im);
	free(term);
}

static int session_event(struct kmscon_session *session,
			 struct kmscon_session_event *ev, void *data)
{
	struct kmscon_terminal *term = data;

	switch (ev->type) {
	case KMSCON_SESSION_DISPLAY_NEW:
		add_display(term, ev->disp);
		break;
	case KMSCON_SESSION_DISPLAY_GONE:
		rm_display(term, ev->disp);
		break;
	case KMSCON_SESSION_DISPLAY_REFRESH:
		redraw_all_test(term);
		break;
	case KMSCON_SESSION_ACTIVATE:
		term->awake = true;
		if (!term->opened)
			terminal_open(term);
		redraw_all_test(term);
		break;
	case KMSCON_SESSION_DEACTIVATE:
		term->awake = false;
		break;
	case KMSCON_SESSION_UNREGISTER:
		terminal_destroy(term);
		break;
	}

	return 0;
}

static void pty_input(struct kmscon_pty *pty, const char *u8, size_t len, void *data)
{
	struct kmscon_terminal *term = data;

	if (!len) {
		terminal_close(term);
		terminal_open(term);
	} else {
		tsm_vte_input(term->vte, u8, len);
		redraw_all(term);
	}
}

static void write_event(struct tsm_vte *vte, const char *u8, size_t len,
			void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_pty_write(term->pty, u8, len);
}

static void pty_event(struct ev_fd *fd, int mask, void *data)
{
	struct kmscon_terminal *term = data;

	kmscon_pty_dispatch(term->pty);
}

int kmscon_terminal_register(struct kmscon_session **out,
			     struct kmscon_seat *seat, unsigned int vtnr)
{
	struct kmscon_terminal *term;
	int ret;

	if (!out || !seat)
		return -EINVAL;

	term = malloc(sizeof(*term));
	if (!term)
		return -ENOMEM;

	memset(term, 0, sizeof(*term));
	term->ref = 1;
	term->eloop = kmscon_seat_get_eloop(seat);
	term->input = kmscon_seat_get_input(seat);
	shl_dlist_init(&term->screens);

	term->conf_ctx = kmscon_seat_get_conf(seat);
	term->conf = conf_ctx_get_mem(term->conf_ctx);

	strncpy(term->font_attr.name, term->conf->font_name,
		KMSCON_FONT_MAX_NAME - 1);
	term->font_attr.ppi = term->conf->font_ppi;
	term->font_attr.points = term->conf->font_size;

	ret = tsm_screen_new(&term->console, log_llog, NULL);
	if (ret)
		goto err_free;
	tsm_screen_set_max_sb(term->console, term->conf->sb_size);

	ret = tsm_vte_new(&term->vte, term->console, write_event, term,
			  log_llog, NULL);
	if (ret)
		goto err_con;
	tsm_vte_set_palette(term->vte, term->conf->palette);

	im_new(&term->im);
	im_ime_load (term->im, ime_load_callback);

	im_actived (term->im, 0);

	ret = font_set(term);
	if (ret)
		goto err_vte;

	ret = kmscon_pty_new(&term->pty, pty_input, term);
	if (ret)
		goto err_font;

	kmscon_pty_set_env_reset(term->pty, term->conf->reset_env);

	ret = kmscon_pty_set_term(term->pty, term->conf->term);
	if (ret)
		goto err_pty;

	ret = kmscon_pty_set_colorterm(term->pty, "kmscon");
	if (ret)
		goto err_pty;

	ret = kmscon_pty_set_argv(term->pty, term->conf->argv);
	if (ret)
		goto err_pty;

	ret = kmscon_pty_set_seat(term->pty, kmscon_seat_get_name(seat));
	if (ret)
		goto err_pty;

	if (vtnr > 0) {
		ret = kmscon_pty_set_vtnr(term->pty, vtnr);
		if (ret)
			goto err_pty;
	}

	ret = ev_eloop_new_fd(term->eloop, &term->ptyfd,
			      kmscon_pty_get_fd(term->pty),
			      EV_READABLE, pty_event, term);
	if (ret)
		goto err_pty;

	ret = uterm_input_register_cb(term->input, input_event, term);
	if (ret)
		goto err_ptyfd;

	ret = kmscon_seat_register_session(seat, &term->session, session_event,
					   term);
	if (ret) {
		log_error("cannot register session for terminal: %d", ret);
		goto err_input;
	}

	ev_eloop_ref(term->eloop);
	uterm_input_ref(term->input);

	term->nn_sock = nn_socket (AF_SP, NN_PAIR);
	if (term->nn_sock < 0)
		goto err_uterm;
        ret = nn_bind (term->nn_sock, "tcp://*:7788");
	if (ret < 0)
		goto err_socket;
	int	fd;
	size_t	sz = sizeof (fd);
	ret = nn_getsockopt (term->nn_sock, NN_SOL_SOCKET, NN_RCVFD, &fd, &sz);
	if (ret < 0)
		goto err_socket;
	ret = ev_eloop_new_fd (term->eloop, &term->fd, fd, EV_READABLE, nn_callback, term);
	if (ret < 0)
		goto err_socket;

        term->controled = 1;

        struct itimerspec spec;
        spec.it_interval.tv_sec = 1;
        spec.it_interval.tv_nsec = 0;
        spec.it_value.tv_sec = 1;
        spec.it_value.tv_nsec = 0;
        ret = ev_eloop_new_timer(term->eloop, &term->putong,
                        &spec, putong_callback,
                        term);

	*out = term->session;
	log_debug("new terminal object %p", term);
	return 0;

err_socket:
	nn_close (term->nn_sock);
err_uterm:
	ev_eloop_unref (term->eloop);
	uterm_input_unref (term->input);

err_input:
	uterm_input_unregister_cb(term->input, input_event, term);
err_ptyfd:
	ev_eloop_rm_fd(term->ptyfd);
err_pty:
	kmscon_pty_unref(term->pty);
err_font:
	kmscon_font_unref(term->bold_font);
	kmscon_font_unref(term->font);
err_vte:
	im_destroy (term->im);
	tsm_vte_unref(term->vte);
err_con:
	tsm_screen_unref(term->console);
err_free:
	free(term);
	return ret;
}
