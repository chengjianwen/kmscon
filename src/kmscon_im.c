/**
 * im - Input Method
 * 这仅仅是输入法的一个逻辑实现。
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
 * Input Method
 * 中文输入法用于将无法直接输入到计算机系统中的中文语言符号输入到计算机系统中。这里仅仅
 * 提供输入法的一个逻辑实现，需要用户自行提供界面渲染功能。
 *
 * 中文输入法的结构为：
 *
 *              preedit
 *           ↗     ↓     ↘ 
 *      INPUT     ime     OUTPUT
 *           ↘     ↓     ↗
 *             candidates
 *
 * 中文输入法im包括preedit、candidates和ime三个部分，preedit类似于GUI中的input部件，
 * 它维护一个可由键盘直接输入的ascii符号组成的数组；candidates则是一个由预选符号组成
 * 的数组，以及一个当前所选符号的偏移量；ime是一个对照表，提供了从preedit字符串到
 * candidates的映射关系。
 *
 * preedit和candidates的渲染通过im_preedit_draw_cb和im_candidates_draw_cb来提供。
 * ime需要通过im_ime_load_cb提供load方法。
 * INPUT和OUTPUT需要通过im_keyboard和im_output_cb实现。
 *
 * 目前实现的ime为pinyin，参考kmscon_pinyin.h文件。
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include "shl_array.h"
#include "shl_log.h"
#include "kmscon_utf8.h"
#include "kmscon_im.h"

#define	LOG_SUBSYSTEM	"im"

struct im {
	struct shl_array *preedit;
	struct shl_array *candidates;
	struct shl_array *pinyin;
	int	selected;
	int	actived;
};

void im_new (struct im **out)
{
	struct im *_im = (struct im *)malloc (sizeof (struct im));

	shl_array_new (&_im->preedit, sizeof (char), 0);
	shl_array_new (&_im->candidates, sizeof (uint32_t), 0);
	_im->selected = -1;
	_im->actived = 0;
	shl_array_new (&_im->pinyin, sizeof (struct im_ime), 0);
	*out = _im;
}

void im_reset (struct im *_im)
{
	_im->preedit->length = 0;
	_im->candidates->length = 0;
	_im->selected = -1;
}

int im_isactive (struct im *_im)
{
	return _im->actived;
}

void im_actived (struct im *_im, int actived)
{
	_im->actived = actived;
}

void im_destroy (struct im *_im)
{
	if (!_im)
		return;

	shl_array_free (_im->preedit);
	shl_array_free (_im->candidates);
	for (int i = 0; i < shl_array_get_length (_im->pinyin); i++)
	{
		struct im_ime *e = SHL_ARRAY_AT (_im->pinyin, struct im_ime, i);
		shl_array_free (e->cand);
	}
	shl_array_free (_im->pinyin);
        free(_im);
}

void im_draw (struct im *_im, im_preedit_draw_cb cb1, im_candidates_draw_cb cb2, int cols, void *data)
{
	if (!_im)
		return;
	uint32_t ucs4;

// clear
	ucs4 = ' ';
	for (int i = 0; i < cols; i++)
		cb1 (_im, i, ucs4, &ucs4, 1, data);

// draw preedit
	for (int i = 0; i < shl_array_get_length (_im->preedit); i++)
	{
		ucs4 = *SHL_ARRAY_AT (_im->preedit, char, i);
		cb1 (_im, i, ucs4, &ucs4, 1, data);
	}
	// cursor
	ucs4 =  '_';
	cb1 (_im, shl_array_get_length (_im->preedit), ucs4, &ucs4, 1, data);

// draw candidates
	// 最多可显示的符号数量
	int     max_size = (cols - PREEDIT_WIDTH) / 2 - 1;
	//	第一个显示的符号
	int	first = _im->selected - max_size;
	if (first < 0)
		first = 0;
	for (int i = first; i < shl_array_get_length (_im->candidates); i++)
	{
		ucs4 = *SHL_ARRAY_AT (_im->candidates, uint32_t, i);
		cb2 (_im, i - first, ucs4, &ucs4, 1, _im->selected == i ? true : false, data);
	}
}

void im_ime_load (struct im *_im, im_ime_load_cb cb)
{
	if (!_im || !_im->pinyin)
		return;
	cb (_im->pinyin);

	int len = shl_array_get_length (_im->pinyin);
	if (!len)
		return;

// ime sorting
	for (int i = 0; i < len - 1; i++)
		for (int j = 0; j < len - i - 1; j++)
		{
			struct im_ime *e1 = SHL_ARRAY_AT(_im->pinyin, struct im_ime, j);
			struct im_ime *e2 = SHL_ARRAY_AT(_im->pinyin, struct im_ime, j + 1);
			if (strcmp (e1->pre, e2->pre) > 0)
			{
				struct im_ime *e_tmp= (struct im_ime *)malloc(sizeof (struct im_ime));
				memcpy (e_tmp, e1, sizeof(*e1));
				memcpy (e1, e2, sizeof(*e1));
				memcpy (e2, e_tmp, sizeof(*e1));
				free (e_tmp);
			}
		}
}

void im_keyboard (struct im *_im, int keycode, im_output_cb cb, bool *handled, void *data)
{
	bool changed = false;
	size_t	len;
	char *u8;
	switch (keycode)
	{
		case KEY_A:
			shl_array_push (_im->preedit, &"a");
			changed = true;
			break;
		case KEY_B:
			shl_array_push (_im->preedit,  &"b");
			changed = true;
			break;
		case KEY_C:
			shl_array_push (_im->preedit,  &"c");
			changed = true;
			break;
		case KEY_D:
			shl_array_push (_im->preedit,  &"d");
			changed = true;
			break;
		case KEY_E:
			shl_array_push (_im->preedit,  &"e");
			changed = true;
			break;
		case KEY_F:
			shl_array_push (_im->preedit,  &"f");
			changed = true;
			break;
		case KEY_G:
			shl_array_push (_im->preedit,  &"g");
			changed = true;
			break;
		case KEY_H:
			shl_array_push (_im->preedit,  &"h");
			changed = true;
			break;
		case KEY_I:
			shl_array_push (_im->preedit,  &"i");
			changed = true;
			break;
		case KEY_J:
			shl_array_push (_im->preedit,  &"j");
			changed = true;
			break;
		case KEY_K:
			shl_array_push (_im->preedit,  &"k");
			changed = true;
			break;
		case KEY_L:
			shl_array_push (_im->preedit,  &"l");
			changed = true;
			break;
		case KEY_M:
			shl_array_push (_im->preedit,  &"m");
			changed = true;
			break;
		case KEY_N:
			shl_array_push (_im->preedit,  &"n");
			changed = true;
			break;
		case KEY_O:
			shl_array_push (_im->preedit,  &"o");
			changed = true;
			break;
		case KEY_P:
			shl_array_push (_im->preedit,  &"p");
			changed = true;
			break;
		case KEY_Q:
			shl_array_push (_im->preedit,  &"q");
			changed = true;
			break;
		case KEY_R:
			shl_array_push (_im->preedit,  &"r");
			changed = true;
			break;
		case KEY_S:
			shl_array_push (_im->preedit,  &"s");
			changed = true;
			break;
		case KEY_T:
			shl_array_push (_im->preedit,  &"t");
			changed = true;
			break;
		case KEY_U:
			shl_array_push (_im->preedit,  &"u");
			changed = true;
			break;
		case KEY_V:
			shl_array_push (_im->preedit,  &"v");
			changed = true;
			break;
		case KEY_W:
			shl_array_push (_im->preedit,  &"w");
			changed = true;
			break;
		case KEY_X:
			shl_array_push (_im->preedit,  &"x");
			changed = true;
			break;
		case KEY_Y:
			shl_array_push (_im->preedit,  &"y");
			changed = true;
			break;
		case KEY_Z:
			shl_array_push (_im->preedit,  &"z");
			changed = true;
			break;
		case KEY_SPACE:
			if (_im->selected >= 0)
			{
				u8 = tsm_ucs4_to_utf8_alloc (SHL_ARRAY_AT(_im->candidates, uint32_t, _im->selected), 1, &len);
				cb (u8, len, data);
				free (u8);
				_im->preedit->length = 0;
				changed = true;
			}
			break;
		case KEY_RIGHT:
			if (_im->selected < shl_array_get_length (_im->candidates) - 1)
			{
				_im->selected++;
				changed = false;
				*handled = true;
			}
			break;
		case KEY_LEFT:
			if (_im->selected > 0)
			{
				_im->selected--;
				changed = false;
				*handled = true;
			}
			break;
		case KEY_HOME:
			if (_im->selected > 0)
			{
				_im->selected = 0;
				changed = false;
				*handled = true;
			}
			break;
		case KEY_END:
			if (_im->selected < shl_array_get_length (_im->candidates) - 1)
			{
				_im->selected = shl_array_get_length (_im->candidates) - 1;;
				changed = false;
				*handled = true;
			}
			break;
		case KEY_ENTER:
			if (shl_array_get_length (_im->preedit))
			{
				cb (shl_array_get_array (_im->preedit), shl_array_get_length (_im->preedit), data);
				_im->preedit->length = 0;
				changed = true;
				*handled = true;
			}
			break;
		case KEY_ESC:
			im_reset (_im);
			changed = false;
			*handled = true;
			break;
		case KEY_BACKSPACE:
			if (shl_array_get_length(_im->preedit))
			{
				shl_array_pop (_im->preedit);
				changed = true;
				*handled = true;
			}
			break;
	}

	if (!changed)
		return;

	_im->candidates->length = 0;
	_im->selected = -1;
	*handled = true;

	if (!_im->preedit->length)
		return;

	// 字典检索，采用二分法
	int min = 0;
	int max = _im->pinyin->length - 1;
	int mid;
	while (min <= max)
	{
		mid = min + (max - min) / 2;
		struct im_ime *e = SHL_ARRAY_AT(_im->pinyin, struct im_ime, mid);
		char *s = strndup (shl_array_get_array (_im->preedit), shl_array_get_length (_im->preedit));
		int ret = strcmp(s, e->pre);
		free (s);
		if (ret > 0)
			min = mid + 1;
		else if (ret < 0)
			max = mid - 1;
		else
		{
			uint32_t	*ch;
			for (int i = 0; i < shl_array_get_length(e->cand); i++)
			{
				ch = SHL_ARRAY_AT(e->cand, uint32_t, i);
				shl_array_push (_im->candidates, ch);
			}
			_im->selected = 0;
			break;
		}
	}
}
