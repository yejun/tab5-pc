// The web server: a file manager for the PC's disks and the SD card, and the PC's controls, at http://<tab5>/.
//
//   GET    /                                    the page (web_page.h)
//   GET    /api/status                          JSON: PC state, volume, brightness, disks, open disk, WiFi
//   POST   /api/pc?do=pause|resume|reset|done|stop|restart   done: close the open disk (the PC continues, restarted
//                                               if changed); stop: the PC stops until the Tab5 restarts, and its
//                                               images can be deleted or replaced; restart: the whole Tab5 (after
//                                               changing tiny386.ini, or to start a stopped PC; WiFi comes back on)
//   POST   /api/set?volume=0-255&brightness=0-255
//   POST   /api/clock?utc=<seconds>&tz=<POSIX TZ>  the Tab5's clock and time zone, from the browser (the page, when
//                                               opened: clock.h)
//   GET    /api/ls?vol=V&path=P                 JSON list of a directory
//   GET    /api/file?vol=V&path=P               download
//   PUT    /api/file?vol=V&path=P&offset=N[&size=S]  upload a chunk (<= 256 KB) of a file at N; 0 starts it (folders on
//                                               the way are made)
//   DELETE /api/file?vol=V&path=P               delete a file, or a folder with everything in it
//   POST   /api/mkdir?vol=V&path=P
// V is "sd" (the card) or "hda".."hdd" (the PC's hard disks: opening one pauses the PC, see disk_access.h).
// On the card, the images the PC has open can't be written or deleted, unless it is stopped.
// The server runs on core 0 (core 1 is the PC's).
#pragma once
#include <esp_http_server.h>
#include <esp_cache.h>
#include <esp_vfs_fat.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "web_page.h"

static httpd_handle_t g_httpd = nullptr;
static uint8_t g_brightness = 160;
static PCConfig g_conf;          // the machine as configured (disk image paths)

static void set_brightness(int v) {
  g_brightness = (uint8_t)constrain(v, 8, 255);
  M5.Display.setBrightness(g_brightness);
  g_prefs.putUChar("bri", g_brightness);
}

// ---- helpers ------------------------------------------------------------------------------------------------

static String url_decode(const char* s) {
  String out;
  for (; *s; ++s) {
    if (*s == '+') out += ' ';
    else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
      const char hex[3] = {s[1], s[2], 0};
      out += (char)strtol(hex, nullptr, 16);
      s += 2;
    } else out += *s;
  }
  return out;
}

static String query(httpd_req_t* req, const char* key) {
  char q[600], v[512];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return "";
  if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return "";
  return url_decode(v);
}

static String json_str(const String& s) {
  String o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += c;
  }
  return o + "\"";
}

static esp_err_t send_json(httpd_req_t* req, const String& body) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.c_str(), body.length());
}

static esp_err_t send_error(httpd_req_t* req, const char* status, const String& msg) {
  httpd_resp_set_status(req, status);
  return send_json(req, "{\"error\":" + json_str(msg) + "}");
}

static bool path_ok(const String& p) {
  return p.startsWith("/") && p.indexOf("/../") < 0 && !p.endsWith("/..") && p.indexOf('\\') < 0;
}

// The local path for vol + path, opening a PC disk if need be (with g_disk_lock held). Empty on error.
static String resolve(httpd_req_t* req, String& err) {
  const String vol = query(req, "vol");
  String path = query(req, "path");
  if (path.isEmpty()) path = "/";
  if (!path_ok(path)) { err = "bad path"; return ""; }
  while (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);
  if (vol == "sd") return path == "/" ? String("/sdcard") : "/sdcard" + path;
  if (vol.length() == 3 && vol.startsWith("hd") && vol[2] >= 'a' && vol[2] <= 'd') {
    if (g_pc_stopped) { err = "the PC is stopped (its disks may be gone): start it first"; return ""; }
    const char* mount = disk_open(vol[2] - 'a', err);
    if (!mount) return "";
    return path == "/" ? String(mount) : String(mount) + path;
  }
  err = "unknown volume";
  return "";
}

// Both VFS and the raw FatFs calls below address this SD_MMC volume.
static bool card_ffpath(char (&out)[600], const char* path, size_t length) {
  const int n = snprintf(out, sizeof(out), "%u:%.*s", SDHandle::pdrv(),
                         (int)(length - 7), path + 7);  // skip "/sdcard"
  return n >= 0 && (size_t)n < sizeof(out);
}

// FatFs can open a directory by its long name or its 8.3 alias. Its first cluster
// identifies it by either name on this card (including zero for the root).
static bool card_dir_cluster(const char* path, size_t length, DWORD& cluster) {
  char ffpath[600];
  FF_DIR dir;
  if (!card_ffpath(ffpath, path, length) || f_opendir(&dir, ffpath) != FR_OK) return false;
  cluster = dir.obj.sclust;
  return f_closedir(&dir) == FR_OK;
}

static FRESULT card_stat(const char* path, FILINFO& info) {
  char ffpath[600];
  if (!card_ffpath(ffpath, path, strlen(path))) return FR_INVALID_NAME;
  return f_stat(ffpath, &info);
}

static const char* configured_image(int slot) {
  return slot < 4 ? g_conf.disks[slot] : g_conf.fdd[slot - 4];
}

// With a DOS alias, f_stat may return the alias itself in fname. The short
// name identifies the entry in either spelling. exFAT has only the stored name.
static const char* entry_name(const FILINFO& info) {
  return *info.altname ? info.altname : info.fname;
}

// Resolve configured images once per listing, under DiskLock so a CD cannot change.
struct ActiveImages {
  String names[6];
  int count = 0;

  bool load(const char* dir, size_t length) {
    if (g_pc_stopped || length < 7 || strncmp(dir, "/sdcard", 7) != 0) return true;
    DWORD target;
    if (!card_dir_cluster(dir, length, target)) return false;
    for (int i = 0; i < 6; ++i) {
      const char* image = configured_image(i);
      if (!image || strncmp(image, "/sdcard/", 8) != 0) continue;
      const char* leaf = image + strlen(image);
      while (leaf > image && leaf[-1] != '/' && leaf[-1] != '\\') --leaf;
      DWORD parent;
      if (!card_dir_cluster(image, leaf - image - 1, parent)) return false;
      if (parent != target) continue;
      FILINFO info;
      if (card_stat(image, info) != FR_OK || !names[count].concat(entry_name(info))) return false;
      ++count;
    }
    return true;
  }

  bool contains(const char* name) const {
    for (int i = 0; i < count; ++i)
      if (names[i] == name) return true;
    return false;
  }

  // For a listing: the names readdir gives these images (their long names), from one pass over the folder. (A
  // lookup of every entry instead read the folder once an entry: 151 entries took 5.1 s to list, from 0.9 s.)
  String shown[6];
  void find_shown(const char* dir, size_t length) {
    char ffpath[600];
    FF_DIR d;
    if (!count || !card_ffpath(ffpath, dir, length) || f_opendir(&d, ffpath) != FR_OK) return;
    FILINFO info;
    for (int left = count; left && f_readdir(&d, &info) == FR_OK && info.fname[0];) {
      if (info.fattrib & AM_DIR) continue;
      for (int i = 0; i < count; ++i)
        if (shown[i].isEmpty() && names[i] == entry_name(info)) { shown[i] = info.fname; --left; break; }
    }
    f_closedir(&d);
  }
  bool shows(const char* name) const {
    for (int i = 0; i < count; ++i)
      if (shown[i] == name) return true;
    return false;
  }
};

static String g_web_free_path;   // the card file last found not to be one of the PC's images (image_access_error)

// A missing destination is normal for a new upload. Other lookup failures must
// stop the operation before an existing image could be opened or truncated.
static const char* image_access_error(const String& path) {
  if (g_pc_stopped) return nullptr;
  for (int i = 0; i < 6; ++i) {
    const char* image = configured_image(i);
    if (image && path.equalsIgnoreCase(image)) return "the PC has this image open";
  }
  if (!path.startsWith("/sdcard/")) return nullptr;
  // The path found free last time is free still: an upload's chunks each come here, and the lookups below read
  // some twenty directory sectors (about 45 ms a chunk, an eighth of an upload's time). What the PC has open
  // changes with a disc change alone (cd_change forgets the path; the machine's other images are for as long as
  // the firmware runs, and a stopped PC stays stopped), and a file that is not one of its images cannot become
  // one. (All of this with the disk lock held, as every handler and cd_change.)
  if (path == g_web_free_path) return nullptr;
  FILINFO info;
  const FRESULT result = card_stat(path.c_str(), info);
  const char* msg = nullptr;
  // (no such file: a new upload; a folder: card_dir_deletable's)
  if (result != FR_NO_FILE && result != FR_NO_PATH && !(result == FR_OK && (info.fattrib & AM_DIR))) {
    ActiveImages active;
    if (result != FR_OK || !active.load(path.c_str(), path.lastIndexOf('/')))
      msg = "could not check whether the PC has this image open";
    else if (active.contains(entry_name(info))) msg = "the PC has this image open";
  }
  if (!msg) g_web_free_path = path;
  return msg;
}

// Check before remove_tree touches the first sibling. If a directory on the way
// to a configured image cannot be inspected, leave the requested tree intact.
static bool card_dir_deletable(const String& path) {
  DWORD target;
  if (!card_dir_cluster(path.c_str(), path.length(), target) || target == 0) return false;
  if (g_pc_stopped) return true;
  for (int i = 0; i < 6; ++i) {
    const char* image = configured_image(i);
    if (!image || strncmp(image, "/sdcard/", 8) != 0) continue;
    for (const char* slash = strpbrk(image + 8, "/\\"); slash; slash = strpbrk(slash + 1, "/\\")) {
      DWORD parent;
      if (!card_dir_cluster(image, slash - image, parent) || parent == target) return false;
    }
  }
  return true;
}

static void mkdirs(const String& dir) {   // every folder on the way to dir
  for (int i = 1; i <= (int)dir.length(); ++i)
    if (i == (int)dir.length() || dir[i] == '/') mkdir(dir.substring(0, i).c_str(), 0777);
}

static bool remove_tree(const String& p) {
  struct stat st;
  if (stat(p.c_str(), &st) != 0) return false;
  if (!S_ISDIR(st.st_mode)) return unlink(p.c_str()) == 0;
  DIR* d = opendir(p.c_str());
  if (d) {
    while (dirent* e = readdir(d)) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      remove_tree(p + "/" + e->d_name);
    }
    closedir(d);
  }
  return rmdir(p.c_str()) == 0;
}

struct DiskLock {   // g_disk_lock for the life of a handler
  DiskLock() { xSemaphoreTake(g_disk_lock, portMAX_DELAY); }
  ~DiskLock() { if (g_disk.drive >= 0) g_disk.last_use = esp_timer_get_time() / 1000; xSemaphoreGive(g_disk_lock); }
};

// ---- handlers -----------------------------------------------------------------------------------------------

static esp_err_t h_page(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, WEB_PAGE, HTTPD_RESP_USE_STRLEN);
}

static const char* pc_state();

static esp_err_t h_status(httpd_req_t* req) {
  String j = "{\"version\":" + json_str(FIRMWARE_VERSION) + ",\"pc\":" + json_str(pc_state()) +
             ",\"volume\":" + g_volume + ",\"brightness\":" + g_brightness + ",\"open\":";
  j += g_disk.drive >= 0 ? json_str(String("hd") + (char)('a' + g_disk.drive)) : String("null");
  j += ",\"changed\":" + String(g_disk.changed ? "true" : "false");
  j += ",\"idle_close_s\":" + String(DISK_IDLE_MS / 1000);
  j += ",\"disks\":[";
  bool first = true;
  for (int i = 0; i < 4; ++i) {
    if (g_pc_stopped || !g_conf.disks[i] || g_conf.iscd[i] || !disk_image(i)) continue;
    j += String(first ? "" : ",") + "{\"vol\":\"hd" + (char)('a' + i) + "\",\"file\":" + json_str(g_conf.disks[i]) + "}";
    first = false;
  }
  j += "],\"ssid\":" + json_str(g_ssid) + ",\"ip\":" + json_str(WiFi.localIP().toString()) + "}";
  return send_json(req, j);
}

static void pc_set_paused(bool on);

// Restart the Tab5 (to start a stopped PC, or with a changed tiny386.ini). WiFi, if on, comes back on: the web page
// that asked finds the Tab5 again.
static void restart_tab5() {
  { DiskLock l; disk_close(); }
  if (g_wifi != WIFI_DISABLED) g_prefs.putBool("wifiback", true);
  delay(300);
  tab5_restart();
}

// Stop the PC until the Tab5 restarts, like switching it off: what it wrote to its images is on the card, and it
// never touches them again, so they can be deleted or replaced while it still has them open (the card's FatFs has
// no file locks: CONFIG_FATFS_FS_LOCK=0).
static void pc_stop() {
  DiskLock l;
  if (g_pc_stopped) return;
  pc_hold(true);   // for good (taken before disk_close() lets go of its own hold)
  disk_close();
  for (int i = 0; i < 4; ++i)
    if (FILE* f = disk_image(i)) { fflush(f); fsync(fileno(f)); }
  g_pc_stopped = true;
  Serial.println("pc: stopped (until the Tab5 restarts)");
}

static esp_err_t h_pc(httpd_req_t* req) {
  const String what = query(req, "do");
  if (g_pc_stopped && what != "restart" && what != "stop")
    return send_error(req, "409 Conflict", "the PC is stopped: start it (restart the Tab5)");
  if (what == "stop") pc_stop();
  else if (what == "pause") pc_set_paused(true);
  else if (what == "resume") pc_set_paused(false);
  else if (what == "reset") { pc_cold_reset(); pc_set_paused(false); }
  else if (what == "done") { DiskLock l; disk_close(); }
  else if (what == "restart") {
    send_json(req, "{\"ok\":true}");
    restart_tab5();
  }
  else return send_error(req, "400 Bad Request", "do: pause, resume, reset, done, stop or restart");
  return h_status(req);
}

static esp_err_t h_clock(httpd_req_t* req) {
  clock_set((time_t)strtoll(query(req, "utc").c_str(), nullptr, 10), query(req, "tz"));
  return send_json(req, "{\"ok\":true}");
}

static esp_err_t h_set(httpd_req_t* req) {
  const String v = query(req, "volume"), b = query(req, "brightness");
  if (!v.isEmpty()) set_volume(v.toInt());
  if (!b.isEmpty()) set_brightness(b.toInt());
  return h_status(req);
}

static esp_err_t h_ls(httpd_req_t* req) {
  DiskLock l;
  String err;
  const String dir = resolve(req, err);
  if (dir.isEmpty()) return send_error(req, "400 Bad Request", err);
  DIR* d = opendir(dir.c_str());
  if (!d) return send_error(req, "404 Not Found", "no such folder");
  // (the "busy" marks are a hint for the page: where an image or an entry can't be looked up, the folder is listed
  // without them; reading, writing and deleting check for themselves, image_access_error)
  ActiveImages active;
  active.load(dir.c_str(), dir.length());
  active.find_shown(dir.c_str(), dir.length());
  String j = "[";
  bool first = true;
  while (dirent* e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    struct stat st = {};
    const String p = dir + "/" + e->d_name;
    stat(p.c_str(), &st);
    const bool busy = !S_ISDIR(st.st_mode) && active.shows(e->d_name);
    j += String(first ? "" : ",") + "{\"n\":" + json_str(e->d_name) + ",\"d\":" + (S_ISDIR(st.st_mode) ? "1" : "0") +
         ",\"s\":" + String((uint32_t)st.st_size) + ",\"t\":" + String((uint32_t)st.st_mtime) +
         (busy ? ",\"busy\":1" : "") + "}";
    first = false;
  }
  closedir(d);
  return send_json(req, j + "]");
}

static esp_err_t h_get(httpd_req_t* req) {
  DiskLock l;
  String err;
  const String p = resolve(req, err);
  if (p.isEmpty()) return send_error(req, "400 Bad Request", err);
  if (const char* msg = image_access_error(p)) return send_error(req, "409 Conflict", msg);
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) return send_error(req, "404 Not Found", "no such file");
  httpd_resp_set_type(req, "application/octet-stream");
  // PSRAM (internal RAM is what WiFi runs short of), whole cache lines: the card's DMA reads into it
  static char* buf = nullptr;
  static constexpr size_t BUF = 16384;
  if (!buf) buf = (char*)heap_caps_aligned_alloc(128, BUF, MALLOC_CAP_SPIRAM);
  if (!buf) { fclose(f); return send_error(req, "500 Internal Server Error", "no memory"); }
  size_t total = 0;
  for (;;) {
    // a read that fails is tried again before the download is given up
    // (read(), not fread: through stdio's buffer the card was read 512 bytes a command, ~1.1 MB/s)
    ssize_t got;
    for (int tries = 0;; ++tries) {
      {
        JitHold hold;   // (tab5_386.ino: the card's driver's cache operations on PSRAM)
        got = read(fileno(f), buf, BUF);
      }
      if (got >= 0 || tries == 3) break;
      const off_t pos = lseek(fileno(f), total, SEEK_SET);
      if (pos < 0 || (size_t)pos != total) break;
      delay(1);
    }
    if (got < 0) {
      Serial.printf("web: download %s: read failed (errno %d) after %u bytes\n", p.c_str(), errno, (unsigned)total);
      fclose(f);
      return ESP_FAIL;
    }
    if (got == 0) break;
    const esp_err_t e = httpd_resp_send_chunk(req, buf, got);
    if (e != ESP_OK) {
      Serial.printf("web: download %s: send failed (%s) after %u bytes\n", p.c_str(), esp_err_to_name(e), (unsigned)total);
      fclose(f);
      return ESP_FAIL;   // (the connection is closed: the browser sees the download fail, not a shorter file)
    }
    total += got;
  }
  fclose(f);
  return httpd_resp_send_chunk(req, nullptr, 0);
}

// Uploads come in chunks of at most UPLOAD_CHUNK bytes (&offset=N; 0 starts the file). Each is received whole into
// PSRAM, and only then written to the card: the SD card and the WiFi chip share the P4's SDMMC host, and while a
// write holds it, data the server hasn't read piles up in the WiFi driver's few receive buffers (the driver stops when
// they run out). Received at full speed, a chunk never leaves data waiting; meanwhile the browser waits for
// the answer, so nothing arrives during the write.
static constexpr size_t UPLOAD_CHUNK = 256 << 10;   // (1 MB left too little PSRAM for WiFi's packets, which stalled)
static char* g_upload_buf = nullptr;

static char* upload_buffer() {
  if (!g_upload_buf) g_upload_buf = (char*)heap_caps_aligned_alloc(128, UPLOAD_CHUNK, MALLOC_CAP_SPIRAM);
  return g_upload_buf;
}

static esp_err_t h_put(httpd_req_t* req) {
  // A body without a stated length (chunked transfer encoding, or no Content-Length at all) reaches here with
  // content_len 0: taken for an empty upload, it emptied the file at offset 0 (up to 1.7.0).
  if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding") || !httpd_req_get_hdr_value_len(req, "Content-Length")) {
    send_error(req, "411 Length Required", "send the chunk with its Content-Length");
    return ESP_FAIL;   // (and the connection closed, as below: whatever body there is stays unread)
  }
  if (req->content_len > UPLOAD_CHUNK) {   // (and the connection closed: its body, unread, held Wi-Fi's
    send_error(req, "413 Payload Too Large", "send files in chunks of up to 256 KB (&offset=)");   // receive
    return ESP_FAIL;                                                                                // buffers)
  }
  // The offset: digits alone (none: 0), and the chunk's end within what fseek takes, a long (2 GB here). A
  // negative one, or one from 2 GB on, made fseek fail, and the failure's way out deletes the file (up to 1.7.0).
  const String off_s = query(req, "offset");
  char* off_end;
  const unsigned long long off_ull = strtoull(off_s.c_str(), &off_end, 10);
  if (*off_end || (off_s.length() && !isdigit((unsigned char)off_s[0])) || off_ull > (unsigned long long)LONG_MAX - req->content_len) {
    send_error(req, "400 Bad Request", "bad offset (a file's chunks go below 2 GB)");
    return ESP_FAIL;
  }
  const long offset = (long)off_ull;
  if (req->content_len && !upload_buffer()) {
    send_error(req, "503 Service Unavailable", "no memory for upload");
    return ESP_FAIL;   // close with the body unread
  }
  size_t got = 0;   // receive the whole chunk before touching the card
  while (got < req->content_len) {
    const int n = httpd_req_recv(req, g_upload_buf + got, req->content_len - got);
    if (n <= 0) return ESP_FAIL;   // disconnected or idle for the receive timeout
    got += n;
  }
  DiskLock l;
  String err;
  const String p = resolve(req, err);
  if (p.isEmpty()) return send_error(req, "400 Bad Request", err);
  if (const char* msg = image_access_error(p)) return send_error(req, "409 Conflict", msg);
  if (offset == 0) mkdirs(p.substring(0, p.lastIndexOf('/')));
  bool fresh = offset == 0;
  // A big file for the card (a disk image, with its size given on the first chunk) goes where there is room for all
  // of it in one piece, so that the PC reads it as raw card sectors (disk_access.h): a new file, with FatFs's next
  // allocation set to the start of a free stretch that long (f_expand, "prepare"); written in order, it fills it.
  const uint64_t size = strtoull(query(req, "size").c_str(), nullptr, 10);
  if (fresh && size >= (1 << 20) && p.startsWith("/sdcard/")) {
    unlink(p.c_str());
    if (esp_vfs_fat_create_contiguous_file("/sdcard", p.c_str(), size, false) == ESP_OK) fresh = false;
  }
  FILE* f = fopen(p.c_str(), fresh ? "wb" : "r+b");
  if (!f) return offset ? send_error(req, "409 Conflict", "no file to go on with: its first chunk has offset 0")
                        : send_error(req, "500 Internal Server Error", "can't create the file (disk full, or a bad name?)");
  if (offset) {   // a chunk may follow the last one or repeat an earlier one, not leave a gap (which the card's
    struct stat st;   // old contents would fill)
    if (fstat(fileno(f), &st) != 0 || offset > st.st_size) {
      fclose(f);
      return send_error(req, "409 Conflict", "offset beyond the end of the file: send its chunks in order");
    }
  }
  // the card's DMA reads the buffer from PSRAM: what the CPU wrote must be there, not only in its cache (without,
  // 64-byte lines of the previous chunk ended up on the card now and then)
  bool ok;
  {
    JitHold hold;   // (tab5_386.ino: the write-back, and the card's driver's from PSRAM)
    ok = fseek(f, offset, SEEK_SET) == 0;
    if (ok && got) {
      esp_cache_msync(g_upload_buf, (got + 127) & ~127, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      ok = fwrite(g_upload_buf, 1, got, f) == got;
    }
  }
  if (fclose(f) != 0) ok = false;
  if (!ok) {
    unlink(p.c_str());
    return send_error(req, "500 Internal Server Error", "write failed (disk full?)");
  }
  return send_json(req, "{\"ok\":true}");
}

static esp_err_t h_delete(httpd_req_t* req) {
  DiskLock l;
  String err;
  const String p = resolve(req, err);
  if (p.isEmpty()) return send_error(req, "400 Bad Request", err);
  if (p == "/sdcard" || p.lastIndexOf('/') == 0) return send_error(req, "400 Bad Request", "not the whole disk");
  if (const char* msg = image_access_error(p)) return send_error(req, "409 Conflict", msg);
  struct stat st;
  if (stat(p.c_str(), &st) != 0) return send_error(req, "404 Not Found", "no such file or folder");
  if (S_ISDIR(st.st_mode) && p.startsWith("/sdcard/") && !card_dir_deletable(p))
    return send_error(req, "409 Conflict", "cannot delete this folder: it is protected or its disk-image paths could not be checked");
  if (!remove_tree(p)) return send_error(req, "500 Internal Server Error", "couldn't delete it all");
  return send_json(req, "{\"ok\":true}");
}

static esp_err_t h_mkdir(httpd_req_t* req) {
  DiskLock l;
  String err;
  const String p = resolve(req, err);
  if (p.isEmpty()) return send_error(req, "400 Bad Request", err);
  mkdirs(p);
  return send_json(req, "{\"ok\":true}");
}

static void web_start() {
  if (g_httpd) return;
  upload_buffer();   // reserve PSRAM before WiFi starts serving requests
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.core_id = 0;
  cfg.stack_size = 10240;
  cfg.max_uri_handlers = 12;
  cfg.lru_purge_enable = true;
  cfg.recv_wait_timeout = 30;
  cfg.send_wait_timeout = 30;
  if (httpd_start(&g_httpd, &cfg) != ESP_OK) {
    Serial.println("web: server failed to start");
    return;
  }
  const httpd_uri_t uris[] = {
    {"/", HTTP_GET, h_page, nullptr},
    {"/api/status", HTTP_GET, h_status, nullptr},
    {"/api/pc", HTTP_POST, h_pc, nullptr},
    {"/api/set", HTTP_POST, h_set, nullptr},
    {"/api/clock", HTTP_POST, h_clock, nullptr},
    {"/api/ls", HTTP_GET, h_ls, nullptr},
    {"/api/file", HTTP_GET, h_get, nullptr},
    {"/api/file", HTTP_PUT, h_put, nullptr},
    {"/api/file", HTTP_DELETE, h_delete, nullptr},
    {"/api/mkdir", HTTP_POST, h_mkdir, nullptr},
  };
  for (const auto& u : uris) httpd_register_uri_handler(g_httpd, &u);
  Serial.println("web: server started");
}

static void web_stop() {
  if (!g_httpd) return;
  httpd_stop(g_httpd);
  g_httpd = nullptr;
  Serial.println("web: server stopped");
}
