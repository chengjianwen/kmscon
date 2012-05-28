/*
 * kmscon - VT Emulator
 *
 * Copyright (c) 2011 David Herrmann <dh.herrmann@googlemail.com>
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
 * Virtual Terminal Emulator
 * This is the VT implementation. It is written from scratch. It uses the
 * console subsystem as output and is tightly bound to it. It supports
 * functionality from vt100 up to vt500 series. It doesn't implement an
 * explicitly selected terminal but tries to support the most important commands
 * to be compatible with existing implementations. However, full vt102
 * compatibility is the least that is provided.
 *
 * The main parser in this file controls the parser-state and dispatches the
 * actions to the related handlers. The parser is based on the state-diagram
 * from Paul Williams: http://vt100.net/emu/
 * It is written from scratch, though.
 * This parser is fully compatible up to the vt500 series. It requires UTF-8 and
 * does not support any other input encoding. The G0 and G1 sets are therefore
 * defined as subsets of UTF-8. You may still map G0-G3 into GL, though.
 *
 * However, the CSI/DCS/etc handlers are not designed after a specific VT
 * series. We try to support all vt102 commands but implement several other
 * often used sequences, too. Feel free to add further.
 *
 * See ./doc/vte.txt for more information on this VT-emulator.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>

#include "console.h"
#include "font.h"
#include "log.h"
#include "unicode.h"
#include "vte.h"

#define LOG_SUBSYSTEM "vte"

/* Input parser states */
enum parser_state {
	STATE_NONE,		/* placeholder */
	STATE_GROUND,		/* initial state and ground */
	STATE_ESC,		/* ESC sequence was started */
	STATE_ESC_INT,		/* intermediate escape characters */
	STATE_CSI_ENTRY,	/* starting CSI sequence */
	STATE_CSI_PARAM,	/* CSI parameters */
	STATE_CSI_INT,		/* intermediate CSI characters */
	STATE_CSI_IGNORE,	/* CSI error; ignore this CSI sequence */
	STATE_DCS_ENTRY,	/* starting DCS sequence */
	STATE_DCS_PARAM,	/* DCS parameters */
	STATE_DCS_INT,		/* intermediate DCS characters */
	STATE_DCS_PASS,		/* DCS data passthrough */
	STATE_DCS_IGNORE,	/* DCS error; ignore this DCS sequence */
	STATE_OSC_STRING,	/* parsing OCS sequence */
	STATE_ST_IGNORE,	/* unimplemented seq; ignore until ST */
	STATE_NUM
};

/* Input parser actions */
enum parser_action {
	ACTION_NONE,		/* placeholder */
	ACTION_IGNORE,		/* ignore the character entirely */
	ACTION_PRINT,		/* print the character on the console */
	ACTION_EXECUTE,		/* execute single control character (C0/C1) */
	ACTION_CLEAR,		/* clear current parameter state */
	ACTION_COLLECT,		/* collect intermediate character */
	ACTION_PARAM,		/* collect parameter character */
	ACTION_ESC_DISPATCH,	/* dispatch escape sequence */
	ACTION_CSI_DISPATCH,	/* dispatch csi sequence */
	ACTION_DCS_START,	/* start of DCS data */
	ACTION_DCS_COLLECT,	/* collect DCS data */
	ACTION_DCS_END,		/* end of DCS data */
	ACTION_OSC_START,	/* start of OSC data */
	ACTION_OSC_COLLECT,	/* collect OSC data */
	ACTION_OSC_END,		/* end of OSC data */
	ACTION_NUM
};

/* max CSI arguments */
#define CSI_ARG_MAX 16

/* terminal flags */
#define FLAG_CURSOR_KEY_MODE			0x01
#define FLAG_KEYPAD_APPLICATION_MODE		0x02 /* TODO: toggle on numlock? */
#define FLAG_LINE_FEED_NEW_LINE_MODE		0x04

struct kmscon_vte {
	unsigned long ref;
	struct kmscon_console *con;
	kmscon_vte_write_cb write_cb;
	void *data;

	struct kmscon_utf8_mach *mach;

	unsigned int state;
	unsigned int csi_argc;
	int csi_argv[CSI_ARG_MAX];

	struct font_char_attr cattr;
	unsigned int flags;
};

int kmscon_vte_new(struct kmscon_vte **out, struct kmscon_console *con,
		   kmscon_vte_write_cb write_cb, void *data)
{
	struct kmscon_vte *vte;
	int ret;

	if (!out || !con || !write_cb)
		return -EINVAL;

	vte = malloc(sizeof(*vte));
	if (!vte)
		return -ENOMEM;

	memset(vte, 0, sizeof(*vte));
	vte->ref = 1;
	vte->state = STATE_GROUND;
	vte->con = con;
	vte->write_cb = write_cb;
	vte->data = data;

	vte->cattr.fr = 255;
	vte->cattr.fg = 255;
	vte->cattr.fb = 255;
	vte->cattr.br = 0;
	vte->cattr.bg = 0;
	vte->cattr.bb = 0;
	vte->cattr.bold = 0;
	vte->cattr.underline = 0;
	vte->cattr.inverse = 0;

	ret = kmscon_utf8_mach_new(&vte->mach);
	if (ret)
		goto err_free;

	log_debug("new vte object");
	kmscon_console_ref(vte->con);
	*out = vte;
	return 0;

err_free:
	free(vte);
	return ret;
}

void kmscon_vte_ref(struct kmscon_vte *vte)
{
	if (!vte)
		return;

	vte->ref++;
}

void kmscon_vte_unref(struct kmscon_vte *vte)
{
	if (!vte || !vte->ref)
		return;

	if (--vte->ref)
		return;

	log_debug("destroying vte object");
	kmscon_console_unref(vte->con);
	kmscon_utf8_mach_free(vte->mach);
	free(vte);
}

static void vte_write(struct kmscon_vte *vte, const char *u8, size_t len)
{
	vte->write_cb(vte, u8, len, vte->data);
}

/* execute control character (C0 or C1) */
static void do_execute(struct kmscon_vte *vte, uint32_t ctrl)
{
	switch (ctrl) {
		case 0x00: /* NUL */
			/* Ignore on input */
			break;
		case 0x05: /* ENQ */
			/* Transmit answerback message */
			/* TODO: is there a better answer than ACK?  */
			vte_write(vte, "\x06", 1);
			break;
		case 0x07: /* BEL */
			/* Sound bell tone */
			/* TODO: I always considered this annying, however, we
			 * should at least provide some way to enable it if the
			 * user *really* wants it.
			 */
			break;
		case 0x08: /* BS */
			/* Move cursor one position left */
			kmscon_console_move_left(vte->con, 1);
			break;
		case 0x09: /* HT */
			/* Move to next tab stop or end of line */
			/* TODO */
			break;
		case 0x0a: /* LF */
		case 0x0b: /* VT */
		case 0x0c: /* FF */
			/* Line feed or newline (CR/NL mode) */
			if (vte->flags & FLAG_LINE_FEED_NEW_LINE_MODE)
				kmscon_console_newline(vte->con);
			else
				kmscon_console_move_down(vte->con, 1, true);
			break;
		case 0x0d: /* CR */
			/* Move cursor to left margin */
			kmscon_console_move_line_home(vte->con);
			break;
		case 0x0e: /* SO */
			/* Map G1 character set into GL */
			/* TODO */
			break;
		case 0x0f: /* SI */
			/* Map G0 character set into Gl */
			/* TODO */
			break;
		case 0x11: /* XON */
			/* Resume transmission */
			/* TODO */
			break;
		case 0x13: /* XOFF */
			/* Stop transmission */
			/* TODO */
			break;
		case 0x18: /* CAN */
			/* Cancel escape sequence */
			/* nothing to do here */
			break;
		case 0x1a: /* SUB */
			/* Discard current escape sequence and show err-sym */
			kmscon_console_write(vte->con, 0xbf, &vte->cattr);
			break;
		case 0x1b: /* ESC */
			/* Invokes an escape sequence */
			/* nothing to do here */
			break;
		case 0x1f: /* DEL */
			/* Ignored */
			break;
		case 0x84: /* IND */
			/* Move down one row, perform scroll-up if needed */
			kmscon_console_move_down(vte->con, 1, true);
			break;
		case 0x85: /* NEL */
			/* CR/NL with scroll-up if needed */
			kmscon_console_newline(vte->con);
			break;
		case 0x88: /* HTS */
			/* Set tab stop at current position */
			/* TODO */
			break;
		case 0x8d: /* RI */
			/* Move up one row, perform scroll-down if needed */
			kmscon_console_move_up(vte->con, 1, true);
			break;
		case 0x8e: /* SS2 */
			/* Temporarily map G2 into GL for next char only */
			/* TODO */
			break;
		case 0x8f: /* SS3 */
			/* Temporarily map G3 into GL for next char only */
			/* TODO */
			break;
		case 0x9a: /* DECID */
			/* Send device attributes response like ANSI DA */
			/* TODO*/
			break;
		case 0x9c: /* ST */
			/* End control string */
			/* nothing to do here */
			break;
		default:
			log_warn("unhandled control char %u", ctrl);
	}
}

static void do_clear(struct kmscon_vte *vte)
{
	int i;

	vte->csi_argc = 0;
	for (i = 0; i < CSI_ARG_MAX; ++i)
		vte->csi_argv[i] = -1;
}

static void do_collect(struct kmscon_vte *vte, uint32_t data)
{
}

static void do_param(struct kmscon_vte *vte, uint32_t data)
{
	int new;

	if (data == ';') {
		if (vte->csi_argc < CSI_ARG_MAX)
			vte->csi_argc++;
		return;
	}

	if (vte->csi_argc >= CSI_ARG_MAX)
		return;

	/* avoid integer overflows; max allowed value is 16384 anyway */
	if (vte->csi_argv[vte->csi_argc] > 0xffff)
		return;

	if (data >= '0' && data <= '9') {
		new = vte->csi_argv[vte->csi_argc];
		if (new <= 0)
			new = data - '0';
		else
			new = new * 10 + data - '0';
		vte->csi_argv[vte->csi_argc] = new;
	}
}

static void do_esc(struct kmscon_vte *vte, uint32_t data)
{
	switch (data) {
		case 'D': /* IND */
			/* Move down one row, perform scroll-up if needed */
			kmscon_console_move_down(vte->con, 1, true);
			break;
		case 'E': /* NEL */
			/* CR/NL with scroll-up if needed */
			kmscon_console_newline(vte->con);
			break;
		case 'H': /* HTS */
			/* Set tab stop at current position */
			/* TODO */
			break;
		case 'M': /* RI */
			/* Move up one row, perform scroll-down if needed */
			kmscon_console_move_up(vte->con, 1, true);
			break;
		case 'N': /* SS2 */
			/* Temporarily map G2 into GL for next char only */
			/* TODO */
			break;
		case 'O': /* SS3 */
			/* Temporarily map G3 into GL for next char only */
			/* TODO */
			break;
		case 'Z': /* DECID */
			/* Send device attributes response like ANSI DA */
			/* TODO*/
			break;
		case '\\': /* ST */
			/* End control string */
			/* nothing to do here */
			break;
		default:
			log_warn("unhandled escape seq %u", data);
	}
}

static void do_csi(struct kmscon_vte *vte, uint32_t data)
{
	int num, i;

	if (vte->csi_argc < CSI_ARG_MAX)
		vte->csi_argc++;

	switch (data) {
		case 'A':
			num = vte->csi_argv[0];
			if (num <= 0)
				num = 1;
			kmscon_console_move_up(vte->con, num, false);
			break;
		case 'B':
			num = vte->csi_argv[0];
			if (num <= 0)
				num = 1;
			kmscon_console_move_down(vte->con, num, false);
			break;
		case 'C':
			num = vte->csi_argv[0];
			if (num <= 0)
				num = 1;
			kmscon_console_move_right(vte->con, num);
			break;
		case 'D':
			num = vte->csi_argv[0];
			if (num <= 0)
				num = 1;
			kmscon_console_move_left(vte->con, num);
			break;
		case 'J':
			if (vte->csi_argv[0] <= 0)
				kmscon_console_erase_cursor_to_screen(vte->con);
			else if (vte->csi_argv[0] == 1)
				kmscon_console_erase_screen_to_cursor(vte->con);
			else if (vte->csi_argv[0] == 2)
				kmscon_console_erase_screen(vte->con);
			else
				log_debug("unknown parameter to CSI-J: %d",
					  vte->csi_argv[0]);
			break;
		case 'K':
			if (vte->csi_argv[0] <= 0)
				kmscon_console_erase_cursor_to_end(vte->con);
			else if (vte->csi_argv[0] == 1)
				kmscon_console_erase_home_to_cursor(vte->con);
			else if (vte->csi_argv[0] == 2)
				kmscon_console_erase_current_line(vte->con);
			else
				log_debug("unknown parameter to CSI-K: %d",
					  vte->csi_argv[0]);
			break;
		case 'm':
			for (i = 0; i < CSI_ARG_MAX; ++i) {
				switch (vte->csi_argv[i]) {
				case -1:
					break;
				case 0:
					vte->cattr.fr = 255;
					vte->cattr.fg = 255;
					vte->cattr.fb = 255;
					vte->cattr.br = 0;
					vte->cattr.bg = 0;
					vte->cattr.bb = 0;
					vte->cattr.bold = 0;
					vte->cattr.underline = 0;
					vte->cattr.inverse = 0;
					break;
				case 1:
					vte->cattr.bold = 1;
					break;
				case 4:
					vte->cattr.underline = 1;
					break;
				case 7:
					vte->cattr.inverse = 1;
					break;
				case 22:
					vte->cattr.bold = 0;
					break;
				case 24:
					vte->cattr.underline = 0;
					break;
				case 27:
					vte->cattr.inverse = 0;
					break;
				case 30:
					vte->cattr.fr = 0;
					vte->cattr.fg = 0;
					vte->cattr.fb = 0;
					break;
				case 31:
					vte->cattr.fr = 205;
					vte->cattr.fg = 0;
					vte->cattr.fb = 0;
					break;
				case 32:
					vte->cattr.fr = 0;
					vte->cattr.fg = 205;
					vte->cattr.fb = 0;
					break;
				case 33:
					vte->cattr.fr = 205;
					vte->cattr.fg = 205;
					vte->cattr.fb = 0;
					break;
				case 34:
					vte->cattr.fr = 0;
					vte->cattr.fg = 0;
					vte->cattr.fb = 238;
					break;
				case 35:
					vte->cattr.fr = 205;
					vte->cattr.fg = 0;
					vte->cattr.fb = 205;
					break;
				case 36:
					vte->cattr.fr = 0;
					vte->cattr.fg = 205;
					vte->cattr.fb = 205;
					break;
				case 37:
					vte->cattr.fr = 255;
					vte->cattr.fg = 255;
					vte->cattr.fb = 255;
					break;
				default:
					log_debug("unhandled SGR attr %i",
						  vte->csi_argv[i]);
				}
			}
			break;
		case 'p': /* DECSCL: Compatibility Level */
			if (vte->csi_argv[0] == 61) {
				/* Switching to VT100 compatibility mode. We do
				 * not support this mode, so ignore it. In fact,
				 * we are almost compatible to it, anyway, so
				 * there is no need to explicitely select it. */
			} else if (vte->csi_argv[0] == 62) {
				/* Switching to VT220 compatibility mode. We are
				 * always compatible with this so ignore it.
				 * We always send 7bit controls so we also do
				 * not care for the parameter value here that
				 * select the control-mode. */
			} else {
				log_debug("unhandled DECSCL 'p' CSI %i",
					  vte->csi_argv[0]);
			}
			break;
		default:
			log_debug("unhandled CSI sequence %c", data);
	}
}

/* perform parser action */
static void do_action(struct kmscon_vte *vte, uint32_t data, int action)
{
	kmscon_symbol_t sym;

	switch (action) {
		case ACTION_NONE:
			/* do nothing */
			return;
		case ACTION_IGNORE:
			/* ignore character */
			break;
		case ACTION_PRINT:
			sym = kmscon_symbol_make(data);
			kmscon_console_write(vte->con, sym, &vte->cattr);
			break;
		case ACTION_EXECUTE:
			do_execute(vte, data);
			break;
		case ACTION_CLEAR:
			do_clear(vte);
			break;
		case ACTION_COLLECT:
			do_collect(vte, data);
			break;
		case ACTION_PARAM:
			do_param(vte, data);
			break;
		case ACTION_ESC_DISPATCH:
			do_esc(vte, data);
			break;
		case ACTION_CSI_DISPATCH:
			do_csi(vte, data);
			break;
		case ACTION_DCS_START:
			break;
		case ACTION_DCS_COLLECT:
			break;
		case ACTION_DCS_END:
			break;
		case ACTION_OSC_START:
			break;
		case ACTION_OSC_COLLECT:
			break;
		case ACTION_OSC_END:
			break;
		default:
			log_warn("invalid action %d", action);
	}
}

/* entry actions to be performed when entering the selected state */
static const int entry_action[] = {
	[STATE_CSI_ENTRY] = ACTION_CLEAR,
	[STATE_DCS_ENTRY] = ACTION_CLEAR,
	[STATE_DCS_PASS] = ACTION_DCS_START,
	[STATE_ESC] = ACTION_CLEAR,
	[STATE_OSC_STRING] = ACTION_OSC_START,
	[STATE_NUM] = ACTION_NONE,
};

/* exit actions to be performed when leaving the selected state */
static const int exit_action[] = {
	[STATE_DCS_PASS] = ACTION_DCS_END,
	[STATE_OSC_STRING] = ACTION_OSC_END,
	[STATE_NUM] = ACTION_NONE,
};

/* perform state transision and dispatch related actions */
static void do_trans(struct kmscon_vte *vte, uint32_t data, int state, int act)
{
	if (state != STATE_NONE) {
		/* A state transition occurs. Perform exit-action,
		 * transition-action and entry-action. Even when performing a
		 * transition to the same state as the current state we do this.
		 * Use STATE_NONE if this is not the desired behavior.
		 */
		do_action(vte, data, exit_action[vte->state]);
		do_action(vte, data, act);
		do_action(vte, data, entry_action[state]);
		vte->state = state;
	} else {
		do_action(vte, data, act);
	}
}

/*
 * Escape sequence parser
 * This parses the new input character \data. It performs state transition and
 * calls the right callbacks for each action.
 */
static void parse_data(struct kmscon_vte *vte, uint32_t raw)
{
	/* events that may occur in any state */
	switch (raw) {
		case 0x18:
		case 0x1a:
		case 0x80 ... 0x8f:
		case 0x91 ... 0x97:
		case 0x99:
		case 0x9a:
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_EXECUTE);
			return;
		case 0x1b:
			do_trans(vte, raw, STATE_ESC, ACTION_NONE);
			return;
		case 0x98:
		case 0x9e:
		case 0x9f:
			do_trans(vte, raw, STATE_ST_IGNORE, ACTION_NONE);
			return;
		case 0x90:
			do_trans(vte, raw, STATE_DCS_ENTRY, ACTION_NONE);
			return;
		case 0x9d:
			do_trans(vte, raw, STATE_OSC_STRING, ACTION_NONE);
			return;
		case 0x9b:
			do_trans(vte, raw, STATE_CSI_ENTRY, ACTION_NONE);
			return;
	}

	/* events that depend on the current state */
	switch (vte->state) {
	case STATE_GROUND:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x80 ... 0x8f:
		case 0x91 ... 0x9a:
		case 0x9c:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_PRINT);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_PRINT);
		return;
	case STATE_ESC:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_ESC_INT, ACTION_COLLECT);
			return;
		case 0x30 ... 0x4f:
		case 0x51 ... 0x57:
		case 0x59:
		case 0x5a:
		case 0x5c:
		case 0x60 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_ESC_DISPATCH);
			return;
		case 0x5b:
			do_trans(vte, raw, STATE_CSI_ENTRY, ACTION_NONE);
			return;
		case 0x5d:
			do_trans(vte, raw, STATE_OSC_STRING, ACTION_NONE);
			return;
		case 0x50:
			do_trans(vte, raw, STATE_DCS_ENTRY, ACTION_NONE);
			return;
		case 0x58:
		case 0x5e:
		case 0x5f:
			do_trans(vte, raw, STATE_ST_IGNORE, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_ESC_INT, ACTION_COLLECT);
		return;
	case STATE_ESC_INT:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x30 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_ESC_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
		return;
	case STATE_CSI_ENTRY:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_CSI_INT, ACTION_COLLECT);
			return;
		case 0x3a:
			do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
			return;
		case 0x30 ... 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_CSI_PARAM, ACTION_PARAM);
			return;
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_CSI_PARAM, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_PARAM:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x30 ... 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_NONE, ACTION_PARAM);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x3a:
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_CSI_INT, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_INT:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x30 ... 0x3f:
			do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_IGNORE:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x3f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	case STATE_DCS_ENTRY:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x3a:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_DCS_INT, ACTION_COLLECT);
			return;
		case 0x30 ... 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_DCS_PARAM, ACTION_PARAM);
			return;
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_DCS_PARAM, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_PARAM:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x30 ... 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_NONE, ACTION_PARAM);
			return;
		case 0x3a:
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_DCS_INT, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_INT:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		case 0x30 ... 0x3f:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_PASS:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x20 ... 0x7e:
			do_trans(vte, raw, STATE_NONE, ACTION_DCS_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_DCS_COLLECT);
		return;
	case STATE_DCS_IGNORE:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	case STATE_OSC_STRING:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_OSC_COLLECT);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_OSC_COLLECT);
		return;
	case STATE_ST_IGNORE:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	}

	log_warn("unhandled input %u in state %d", raw, vte->state);
}

void kmscon_vte_input(struct kmscon_vte *vte, const char *u8, size_t len)
{
	int state;
	uint32_t ucs4;
	size_t i;

	if (!vte || !vte->con)
		return;

	for (i = 0; i < len; ++i) {
		state = kmscon_utf8_mach_feed(vte->mach, u8[i]);
		if (state == KMSCON_UTF8_ACCEPT ||
				state == KMSCON_UTF8_REJECT) {
			ucs4 = kmscon_utf8_mach_get(vte->mach);
			parse_data(vte, ucs4);
		}
	}
}

void kmscon_vte_handle_keyboard(struct kmscon_vte *vte,
				const struct uterm_input_event *ev)
{
	kmscon_symbol_t sym;
	size_t len;
	const char *u8;

	if (UTERM_INPUT_HAS_MODS(ev, UTERM_CONTROL_MASK)) {
		switch (ev->keysym) {
		case XK_2:
		case XK_space:
			vte_write(vte, "\x00", 1);
			return;
		case XK_a:
		case XK_A:
			vte_write(vte, "\x01", 1);
			return;
		case XK_b:
		case XK_B:
			vte_write(vte, "\x02", 1);
			return;
		case XK_c:
		case XK_C:
			vte_write(vte, "\x03", 1);
			return;
		case XK_d:
		case XK_D:
			vte_write(vte, "\x04", 1);
			return;
		case XK_e:
		case XK_E:
			vte_write(vte, "\x05", 1);
			return;
		case XK_f:
		case XK_F:
			vte_write(vte, "\x06", 1);
			return;
		case XK_g:
		case XK_G:
			vte_write(vte, "\x07", 1);
			return;
		case XK_h:
		case XK_H:
			vte_write(vte, "\x08", 1);
			return;
		case XK_i:
		case XK_I:
			vte_write(vte, "\x09", 1);
			return;
		case XK_j:
		case XK_J:
			vte_write(vte, "\x0a", 1);
			return;
		case XK_k:
		case XK_K:
			vte_write(vte, "\x0b", 1);
			return;
		case XK_l:
		case XK_L:
			vte_write(vte, "\x0c", 1);
			return;
		case XK_m:
		case XK_M:
			vte_write(vte, "\x0d", 1);
			return;
		case XK_n:
		case XK_N:
			vte_write(vte, "\x0e", 1);
			return;
		case XK_o:
		case XK_O:
			vte_write(vte, "\x0f", 1);
			return;
		case XK_p:
		case XK_P:
			vte_write(vte, "\x10", 1);
			return;
		case XK_q:
		case XK_Q:
			vte_write(vte, "\x11", 1);
			return;
		case XK_r:
		case XK_R:
			vte_write(vte, "\x12", 1);
			return;
		case XK_s:
		case XK_S:
			vte_write(vte, "\x13", 1);
			return;
		case XK_t:
		case XK_T:
			vte_write(vte, "\x14", 1);
			return;
		case XK_u:
		case XK_U:
			vte_write(vte, "\x15", 1);
			return;
		case XK_v:
		case XK_V:
			vte_write(vte, "\x16", 1);
			return;
		case XK_w:
		case XK_W:
			vte_write(vte, "\x17", 1);
			return;
		case XK_x:
		case XK_X:
			vte_write(vte, "\x18", 1);
			return;
		case XK_y:
		case XK_Y:
			vte_write(vte, "\x19", 1);
			return;
		case XK_z:
		case XK_Z:
			vte_write(vte, "\x1a", 1);
			return;
		case XK_3:
		case XK_bracketleft:
		case XK_braceleft:
			vte_write(vte, "\x1b", 1);
			return;
		case XK_4:
		case XK_backslash:
		case XK_bar:
			vte_write(vte, "\x1c", 1);
			return;
		case XK_5:
		case XK_bracketright:
		case XK_braceright:
			vte_write(vte, "\x1d", 1);
			return;
		case XK_6:
		case XK_grave:
		case XK_asciitilde:
			vte_write(vte, "\x1e", 1);
			return;
		case XK_7:
		case XK_slash:
		case XK_question:
			vte_write(vte, "\x1f", 1);
			return;
		case XK_8:
			vte_write(vte, "\x7f", 1);
			return;
		}
	}

	switch (ev->keysym) {
		case XK_BackSpace:
			vte_write(vte, "\x08", 1);
			return;
		case XK_Tab:
		case XK_KP_Tab:
			vte_write(vte, "\x09", 1);
			return;
		case XK_Linefeed:
			vte_write(vte, "\x0a", 1);
			return;
		case XK_Clear:
			vte_write(vte, "\x0b", 1);
			return;
		case XK_Pause:
			vte_write(vte, "\x13", 1);
			return;
		case XK_Scroll_Lock:
			/* TODO: do we need scroll lock impl.? */
			vte_write(vte, "\x14", 1);
			return;
		case XK_Sys_Req:
			vte_write(vte, "\x15", 1);
			return;
		case XK_Escape:
			vte_write(vte, "\x1b", 1);
			return;
		case XK_KP_Enter:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				vte_write(vte, "\eOM", 3);
				return;
			}
			/* fallthrough */
		case XK_Return:
			if (vte->flags & FLAG_LINE_FEED_NEW_LINE_MODE)
				vte_write(vte, "\x0d\x0a", 2);
			else
				vte_write(vte, "\x0d", 1);
			return;
		case XK_Insert:
			vte_write(vte, "\e[2~", 4);
			return;
		case XK_Delete:
			vte_write(vte, "\e[3~", 4);
			return;
		case XK_Page_Up:
			vte_write(vte, "\e[5~", 4);
			return;
		case XK_Page_Down:
			vte_write(vte, "\e[6~", 4);
			return;
		case XK_Up:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				vte_write(vte, "\e[A", 3);
			else
				vte_write(vte, "\e[A", 3);
			return;
		case XK_Down:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				vte_write(vte, "\e[B", 3);
			else
				vte_write(vte, "\e[B", 3);
			return;
		case XK_Right:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				vte_write(vte, "\e[C", 3);
			else
				vte_write(vte, "\e[C", 3);
			return;
		case XK_Left:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				vte_write(vte, "\eOD", 3);
			else
				vte_write(vte, "\e[D", 3);
			return;
		case XK_KP_Insert:
		case XK_KP_0:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOp", 3);
			else
				vte_write(vte, "0", 1);
			return;
		case XK_KP_End:
		case XK_KP_1:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOq", 3);
			else
				vte_write(vte, "1", 1);
			return;
		case XK_KP_Down:
		case XK_KP_2:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOr", 3);
			else
				vte_write(vte, "2", 1);
			return;
		case XK_KP_Page_Down:
		case XK_KP_3:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOs", 3);
			else
				vte_write(vte, "3", 1);
			return;
		case XK_KP_Left:
		case XK_KP_4:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOt", 3);
			else
				vte_write(vte, "4", 1);
			return;
		case XK_KP_Begin:
		case XK_KP_5:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOu", 3);
			else
				vte_write(vte, "5", 1);
			return;
		case XK_KP_Right:
		case XK_KP_6:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOv", 3);
			else
				vte_write(vte, "6", 1);
			return;
		case XK_KP_Home:
		case XK_KP_7:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOw", 3);
			else
				vte_write(vte, "7", 1);
			return;
		case XK_KP_Up:
		case XK_KP_8:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOx", 3);
			else
				vte_write(vte, "8", 1);
			return;
		case XK_KP_Page_Up:
		case XK_KP_9:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOy", 3);
			else
				vte_write(vte, "9", 1);
			return;
		case XK_KP_Subtract:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOm", 3);
			else
				vte_write(vte, "-", 1);
			return;
		case XK_KP_Separator:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOl", 3);
			else
				vte_write(vte, ",", 1);
			return;
		case XK_KP_Delete:
		case XK_KP_Decimal:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOn", 3);
			else
				vte_write(vte, ".", 1);
			return;
		case XK_KP_Equal:
		case XK_KP_Divide:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOj", 3);
			else
				vte_write(vte, "/", 1);
			return;
		case XK_KP_Multiply:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOo", 3);
			else
				vte_write(vte, "*", 1);
			return;
		case XK_KP_Add:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE)
				vte_write(vte, "\eOk", 3);
			else
				vte_write(vte, "+", 1);
			return;
		case XK_F1:
		case XK_KP_F1:
			vte_write(vte, "\eOP", 3);
			return;
		case XK_F2:
		case XK_KP_F2:
			vte_write(vte, "\eOQ", 3);
			return;
		case XK_F3:
		case XK_KP_F3:
			vte_write(vte, "\eOR", 3);
			return;
		case XK_F4:
		case XK_KP_F4:
			vte_write(vte, "\eOS", 3);
			return;
		case XK_KP_Space:
			vte_write(vte, " ", 1);
			return;
		case XK_Home:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				vte_write(vte, "\eOH", 3);
			else
				vte_write(vte, "\e[H", 3);
			return;
		case XK_End:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				vte_write(vte, "\eOF", 3);
			else
				vte_write(vte, "\e[F", 3);
			return;
		case XK_F5:
			vte_write(vte, "\e[15~", 5);
			return;
		case XK_F6:
			vte_write(vte, "\e[17~", 5);
			return;
		case XK_F7:
			vte_write(vte, "\e[18~", 5);
			return;
		case XK_F8:
			vte_write(vte, "\e[19~", 5);
			return;
		case XK_F9:
			vte_write(vte, "\e[20~", 5);
			return;
		case XK_F10:
			vte_write(vte, "\e[21~", 5);
			return;
		case XK_F11:
			vte_write(vte, "\e[23~", 5);
			return;
		case XK_F12:
			vte_write(vte, "\e[24~", 5);
			return;
		case XK_F13:
			vte_write(vte, "\e[25~", 5);
			return;
		case XK_F14:
			vte_write(vte, "\e[26~", 5);
			return;
		case XK_F15:
			vte_write(vte, "\e[28~", 5);
			return;
		case XK_F16:
			vte_write(vte, "\e[29~", 5);
			return;
		case XK_F17:
			vte_write(vte, "\e[31~", 5);
			return;
		case XK_F18:
			vte_write(vte, "\e[32~", 5);
			return;
		case XK_F19:
			vte_write(vte, "\e[33~", 5);
			return;
		case XK_F20:
			vte_write(vte, "\e[34~", 5);
			return;
	}

	if (ev->unicode != UTERM_INPUT_INVALID) {
		sym = kmscon_symbol_make(ev->unicode);
		u8 = kmscon_symbol_get_u8(sym, &len);
		vte_write(vte, u8, len);
		kmscon_symbol_free_u8(u8);
		return;
	}
}
