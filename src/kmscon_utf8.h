#ifndef	TSM_LIBTSM_INT_H
#define TSM_LIBTSM_INT_H
/*
 * 从libtsm扒出来的UTF8解析状态机，源代码位于libtsm-int.h中。
 * 使用这些函数，需要在链接时指定: 
 */
#include <libtsm.h>

struct tsm_utf8_mach;

enum tsm_utf8_mach_state {
	TSM_UTF8_START,
	TSM_UTF8_ACCEPT,
	TSM_UTF8_REJECT,
	TSM_UTF8_EXPECT1,
	TSM_UTF8_EXPECT2,
	TSM_UTF8_EXPECT3,
};

int tsm_utf8_mach_new(struct tsm_utf8_mach **out);
void tsm_utf8_mach_free(struct tsm_utf8_mach *mach);

int tsm_utf8_mach_feed(struct tsm_utf8_mach *mach, char c);
uint32_t tsm_utf8_mach_get(struct tsm_utf8_mach *mach);
void tsm_utf8_mach_reset(struct tsm_utf8_mach *mach);

#endif
