// Included right before i8254.c (by t386_i8254.c). i8254_update_irq() implements the periodic
// modes (2, 3) of the timer's channel 0 and abort()s on any other, which a DOS program can choose (Allegro games set
// mode 0): the whole Tab5 would crash. Instead, the other modes are one-shot: the interrupt comes once, when the
// count runs out after it was loaded (last_irq_count marks "fired"; loading a count clears it). That is the only
// call to abort() in i8254.c. <stdlib.h> must be included before this, so its declaration of abort() is untouched.
#define abort() do { \
        if (s->last_irq_count != 0xFFFFFFFFu && d >= s->count) { \
            if (s->irq != -1) { \
                pit->set_irq(pit->pic, s->irq, 1); \
                pit->set_irq(pit->pic, s->irq, 0); \
            } \
            s->last_irq_count = 0xFFFFFFFFu; \
        } \
    } while (0)
