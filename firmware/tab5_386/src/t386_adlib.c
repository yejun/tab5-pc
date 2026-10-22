// tiny386 core: adlib.c (see tab5_config.h)
#include "tab5_config.h"
// (-Os, as Arduino builds: built with -O3, the four sound files together gave static in the sound)
#include "fmopl.h"

// The PC (core 1) writes the OPL chip's registers while the sound task (core 0) renders its samples: unsynchronised,
// that corrupts the synthesiser's channel state, not just a sample. Every use of the chip goes through a mutex
// (not a critical section, which would render with interrupts off on core 0, where the speaker is fed),
// rendering in short pieces so that the PC never waits long.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t opl_lock;
static void opl_take(void) {
  if (!opl_lock) {   // first use; adlib_new runs before the sound task starts
    static StaticSemaphore_t mem;
    opl_lock = xSemaphoreCreateMutexStatic(&mem);
  }
  xSemaphoreTake(opl_lock, portMAX_DELAY);
}
static void opl_give(void) { xSemaphoreGive(opl_lock); }

static int t386_opl_write(FM_OPL* opl, int a, int v) {
  opl_take();
  const int r = OPLWrite(opl, a, v);
  opl_give();
  return r;
}

static unsigned char t386_opl_read(FM_OPL* opl, int a) {
  opl_take();
  const unsigned char r = OPLRead(opl, a);
  opl_give();
  return r;
}

static int t386_opl_timer_over(FM_OPL* opl, int c) {
  opl_take();
  const int r = OPLTimerOver(opl, c);
  opl_give();
  return r;
}

static void t386_opl_generate(FM_OPL* opl, int16_t* buf, int length) {
  for (int i = 0; i < length; i += 64) {
    opl_take();
    YM3812UpdateOne(opl, buf + i, length - i < 64 ? length - i : 64);
    opl_give();
  }
}

#define OPLWrite t386_opl_write
#define OPLRead t386_opl_read
#define OPLTimerOver t386_opl_timer_over
#define YM3812UpdateOne t386_opl_generate
#include "adlib.c"
