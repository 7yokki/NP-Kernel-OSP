#ifndef NPK_MOUSE_H
#define NPK_MOUSE_H

#include "types.h"

#define NPK_MOUSE_BUTTON_LEFT 0x110U
#define NPK_MOUSE_BUTTON_RIGHT 0x111U
#define NPK_MOUSE_BUTTON_MIDDLE 0x112U

void mouse_init(void);
void mouse_irq(void);
bool mouse_ready(void);

#endif
