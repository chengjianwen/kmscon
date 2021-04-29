#ifndef	IM_H
#define	IM_H

#include "shl_array.h"

// preedit宽度
#define	PREEDIT_WIDTH	10

/*
 * 输入法字典
 *    pre: 由ascii码a-z组成的字符串, '\0'符号结尾
 *   cond: 由ucs4符号组成的数组
 *
 * 当preedit发生变化时，会通过在im_ime中检索，生成candidates。
 */
struct im_ime {
	char *pre;
	struct shl_array *cand;
};
struct im;

// 输出函数
typedef void (*im_output_cb) (const char *, size_t, void *);
// preedit绘制函数
typedef void (*im_preedit_draw_cb) (struct im *, int, uint32_t, uint32_t *, size_t, void *data);
// candidates绘制函数
typedef void (*im_candidates_draw_cb) (struct im *, int, uint32_t, uint32_t *, size_t, bool, void *data);
// ime字典装载函数
typedef void (*im_ime_load_cb) (struct shl_array *);

// 创建IM
void im_new (struct im **out);

// 重置IM
void im_reset (struct im *);

// 是否激活
int im_isactive (struct im *);

// 激活/不激活
void im_actived (struct im *, int);

// 销毁IM
void im_destroy (struct im *);

// 键盘输入处理
void im_keyboard (struct im *, int keycode, im_output_cb, bool *, void *);

// 绘制
void im_draw (struct im *, im_preedit_draw_cb, im_candidates_draw_cb, int, void *data);

// 载入输入法字典
void im_ime_load (struct im *_im, im_ime_load_cb);

#endif
