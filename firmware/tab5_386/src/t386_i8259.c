// tiny386 core: i8259.c (see tab5_config.h)
#include "tab5_config.h"
#pragma GCC optimize("O3")
#include "i8259.c"

// Both PICs' request, mask and in-service registers (serial "cpu").
void t386_pic_report(PicState2* s, char* buf, int len) {
    snprintf(buf, len, "pic irr %02x/%02x imr %02x/%02x isr %02x/%02x", s->pics[0].irr, s->pics[1].irr,
             s->pics[0].imr, s->pics[1].imr, s->pics[0].isr, s->pics[1].isr);
}
