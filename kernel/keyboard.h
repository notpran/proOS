#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stddef.h>

void kb_init(void);
char kb_getchar(void);
int kb_poll(void);
int kb_dump_layout(char *out, size_t max_len);

#define KB_IRQ_LINE 1
#define KB_EVENT_FLAG_RELEASE 0x100u
#define KB_EVENT_FLAG_EXTENDED 0x200u

#define KB_KEY_ARROW_UP ((char)0x80)
#define KB_KEY_ARROW_DOWN ((char)0x81)
#define KB_KEY_ARROW_LEFT ((char)0x82)
#define KB_KEY_ARROW_RIGHT ((char)0x83)

#endif
