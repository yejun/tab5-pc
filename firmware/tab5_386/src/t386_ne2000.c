// tiny386 core: ne2000.c (see tab5_config.h)
#include "tab5_config.h"
#include "ne2000.c"

// Tab5 additions for the guest network (guest_net.h): the card's MAC address (ne2000.c's ESP32 build reads the Wi-Fi
// station's, which this chip doesn't have in its eFuses: all zeros), frames for the guest, frames from it.

// Before the guest starts (the reset copies it into the card's PROM)
void t386_ne2000_set_mac(const uint8_t mac[6])
{
    NE2000State *s = thene2000;
    if (!s)
        return;
    memcpy(s->macaddr, mac, 6);
    ne2000_reset(s);
}

// A frame for the guest, on the PC's core: 0 if the card's receive ring is full (try again later), 1 if it took it
// (or dropped it: card stopped, not for it)
int t386_ne2000_receive(const uint8_t *buf, int size)
{
    NE2000State *s = thene2000;
    if (!s)
        return 1;
    if (!(s->cmd & E8390_STOP) && ne2000_buffer_full(s))
        return 0;
    ne2000_receive(s, buf, size);
    return 1;
}

// Where the guest's frames go (called on the PC's core), or NULL
void t386_ne2000_set_sender(void (*send)(uint8_t *buf, int size))
{
    atomic_store_explicit(&esp32_send_packet, send, memory_order_relaxed);
}

// The guest's driver may clear the card's "received" interrupt for a frame that came in while it was finishing the
// last ones (a new frame lands between two steps of the PC, i.e. inside its handler): that frame sits in the ring
// unannounced, and with real traffic the next frame would announce it. guest_net.h delivers no more once the ring is
// full, so nothing would: when the ring has stayed full for a while, this announces its frames again (a receive
// interrupt, as an edge: the 8259 here is edge-triggered). The driver takes it as a normal one.
void t386_ne2000_kick(void)
{
    NE2000State *s = thene2000;
    if (!s || (s->cmd & E8390_STOP))
        return;
    atomic_fetch_or_explicit(&s->isr, ENISR_RX, memory_order_release);
    s->set_irq(s->pic, s->irq, 0);
    ne2000_update_irq(s);
}

// For serial "net": the card's registers that say where the ring is and what it has announced
int t386_ne2000_state(char *buf, int len)
{
    NE2000State *s = thene2000;
    if (!s)
        return snprintf(buf, len, "no card");
    return snprintf(buf, len, "cmd %02x isr %02x imr %02x ring %02x-%02x write %02x read %02x%s", s->cmd,
                    (unsigned) atomic_load(&s->isr), s->imr, s->start >> 8, s->stop >> 8, s->curpag, s->boundary,
                    ne2000_buffer_full(s) ? " full" : "");
}
