// A Tab5 that crashes at its start, again and again, is of no use and can't be told to do otherwise: its settings
// are read after the start. So crashes in a row are counted (in the preferences: "crashes"), and the start gives
// things up to get through:
//   2 in a row: Wi-Fi is not started (the setting stays: the next good start tries again)
//   4 in a row: the PC stays off too, and the settings open: another machine can be chosen, or this one started
// A start that lasts GUARD_GOOD_MS ends the row. Only restarts by a crash count (a panic, a watchdog), not the
// Tab5's own (a machine chosen, an install) nor the power switch: after those the row is as it was.
// Wi-Fi also isn't started with less DMA-capable internal RAM free than it needs (net.h): its driver stops then.
// A start can also hang: once in about 100 restarts the start of Wi-Fi never returned (cause not established): the
// screen black (the display task is the one that hangs, with the PC held), nothing on serial, and only the reset
// key left. So the display task's start is timed (guard_watch): one that has not reached its second round after
// GUARD_HANG_US is ended as a crash (abort), which the row above counts.
static constexpr int GUARD_NO_WIFI = 2, GUARD_NO_PC = 4;
static constexpr int64_t GUARD_GOOD_MS = 30000;
static constexpr unsigned WIFI_NEEDS = 130000;   // bytes: 122 KB crashed, 127 KB started but joined nothing, 135 KB works
static int g_crashes = 0;            // crashes in a row before this start
static bool g_guard_wifi = false;    // this start: Wi-Fi left off for them
static bool g_guard_pc = false;      // this start: the PC left off for them
static unsigned g_wifi_short = 0;    // Wi-Fi not started: only this much DMA-capable internal RAM was free

static void guard_setup() {   // (the preferences are open: audio_setup)
  const esp_reset_reason_t r = esp_reset_reason();
  g_crashes = g_prefs.getUChar("crashes", 0);
  if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT) {
    if (g_crashes < 200) g_prefs.putUChar("crashes", ++g_crashes);
  }
  g_guard_wifi = g_crashes >= GUARD_NO_WIFI;
  g_guard_pc = g_crashes >= GUARD_NO_PC;
  if (g_crashes)
    Serial.printf("start: %d crash%s in a row before this start%s%s\n", g_crashes, g_crashes == 1 ? "" : "es",
                  g_guard_wifi ? ": Wi-Fi left off" : "", g_guard_pc ? ", the PC left off" : "");
}

static void guard_poll(int64_t now_ms) {   // the start lasted: the row ends
  static bool done = false;
  if (done || now_ms < GUARD_GOOD_MS) return;
  done = true;
  if (g_prefs.getUChar("crashes", 0)) g_prefs.putUChar("crashes", 0);
}

static constexpr uint64_t GUARD_HANG_US = 20000000;   // (it takes 3 s with Wi-Fi, less without)
static esp_timer_handle_t g_guard_watch = nullptr;

static void guard_hung(void*) {   // (the timer's task)
  Serial.println("start: the display task hung at its start: ended as a crash");
  Serial.flush();
  delay(50);
  abort();
}

static void guard_watch(bool on) {
  if (on) {
    const esp_timer_create_args_t a = {guard_hung, nullptr, ESP_TIMER_TASK, "startwatch", true};
    if (esp_timer_create(&a, &g_guard_watch) == ESP_OK) esp_timer_start_once(g_guard_watch, GUARD_HANG_US);
    else g_guard_watch = nullptr;
  } else if (g_guard_watch) {
    esp_timer_stop(g_guard_watch);
    esp_timer_delete(g_guard_watch);
    g_guard_watch = nullptr;
  }
}
