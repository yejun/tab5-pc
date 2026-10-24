// Raw access to disk images on the card (t386_ide.c), set up by the sketch (disk_access.h).
#pragma once
#include <stdint.h>
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sector, card, count;   // image sectors sector.. are card sectors card.., count of them
} T386Run;

int t386_raw_attach(FILE* f, void* card, T386Run* runs, uint32_t nruns, uint32_t sectors);
int t386_raw_rw(FILE* f, int write, void* buf, uint32_t sector, uint32_t count);
void t386_card_lock_setup(void* card);   // (before the card is used from two cores: one command at a time)

#ifdef __cplusplus
}
#endif
