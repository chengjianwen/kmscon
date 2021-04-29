#include <string.h>
#include <libtsm.h>
#include <errno.h>

#include "kmscon_utf8.h"

struct tsm_utf8_mach {
	int state;
	uint32_t ch;
};

int tsm_utf8_mach_new(struct tsm_utf8_mach **out)
{
	struct tsm_utf8_mach *mach;

	if (!out)
		return -EINVAL;

	mach = malloc(sizeof(*mach));
	if (!mach)
		return -ENOMEM;

	memset(mach, 0, sizeof(*mach));
	mach->state = TSM_UTF8_START;

	*out = mach;
	return 0;
}

void tsm_utf8_mach_free(struct tsm_utf8_mach *mach)
{
	if (!mach)
		return;

	free(mach);
}

int tsm_utf8_mach_feed(struct tsm_utf8_mach *mach, char ci)
{
	uint32_t c;

	if (!mach)
		return TSM_UTF8_START;

	c = ci;

	switch (mach->state) {
	case TSM_UTF8_START:
	case TSM_UTF8_ACCEPT:
	case TSM_UTF8_REJECT:
		if (c == 0xC0 || c == 0xC1) {
			/* overlong encoding for ASCII, reject */
			mach->state = TSM_UTF8_REJECT;
		} else if ((c & 0x80) == 0) {
			/* single byte, accept */
			mach->ch = c;
			mach->state = TSM_UTF8_ACCEPT;
		} else if ((c & 0xC0) == 0x80) {
			/* parser out of sync, ignore byte */
			mach->state = TSM_UTF8_START;
		} else if ((c & 0xE0) == 0xC0) {
			/* start of two byte sequence */
			mach->ch = (c & 0x1F) << 6;
			mach->state = TSM_UTF8_EXPECT1;
		} else if ((c & 0xF0) == 0xE0) {
			/* start of three byte sequence */
			mach->ch = (c & 0x0F) << 12;
			mach->state = TSM_UTF8_EXPECT2;
		} else if ((c & 0xF8) == 0xF0) {
			/* start of four byte sequence */
			mach->ch = (c & 0x07) << 18;
			mach->state = TSM_UTF8_EXPECT3;
		} else {
			/* overlong encoding, reject */
			mach->state = TSM_UTF8_REJECT;
		}
		break;
	case TSM_UTF8_EXPECT3:
		mach->ch |= (c & 0x3F) << 12;
		if ((c & 0xC0) == 0x80)
			mach->state = TSM_UTF8_EXPECT2;
		else
			mach->state = TSM_UTF8_REJECT;
		break;
	case TSM_UTF8_EXPECT2:
		mach->ch |= (c & 0x3F) << 6;
		if ((c & 0xC0) == 0x80)
			mach->state = TSM_UTF8_EXPECT1;
		else
			mach->state = TSM_UTF8_REJECT;
		break;
	case TSM_UTF8_EXPECT1:
		mach->ch |= c & 0x3F;
		if ((c & 0xC0) == 0x80)
			mach->state = TSM_UTF8_ACCEPT;
		else
			mach->state = TSM_UTF8_REJECT;
		break;
	default:
		mach->state = TSM_UTF8_REJECT;
		break;
	}

	return mach->state;
}

uint32_t tsm_utf8_mach_get(struct tsm_utf8_mach *mach)
{
	if (!mach || mach->state != TSM_UTF8_ACCEPT)
		return TSM_UCS4_REPLACEMENT;

	return mach->ch;
}

void tsm_utf8_mach_reset(struct tsm_utf8_mach *mach)
{
	if (!mach)
		return;

	mach->state = TSM_UTF8_START;
}
