// USB keyboards and mice on the Tab5's USB-A port (the P4's high-speed USB, ESP-IDF's USB Host Library; through a hub
// too). They are put in the HID boot protocol, which every PC keyboard and mouse speaks (the BIOS relies on it): a
// keyboard sends 8-byte reports of the modifier keys and up to six others held, a mouse its buttons and movement
// (a touchpad on a keyboard is such a mouse, as are combined receivers: keyboard, mouse, media keys).
// Two tasks on core 0 run the host library and this client. A mouse report goes straight to the PC's PS/2 mouse
// (input_put's queue). A keyboard report goes through a queue to the display task (usb_kbd_poll), which turns its
// differences from the last one into key presses and releases for the PC, or typing into the settings panel when
// that is open (LVGL is the display task's alone). Scroll Lock opens and closes the settings panel.
#pragma once
#include <usb/usb_host.h>

static constexpr int USB_KBD_MAX = 4;   // keyboards and mice at once
static constexpr uint8_t HID_SCROLL_LOCK = 0x47;

struct UsbReport { int8_t slot; uint8_t r[8]; };   // r all 0 with slot: the keyboard went away (release its keys)
static QueueHandle_t g_usb_q;
static usb_host_client_handle_t g_usb_client;

struct UsbKbd {
  usb_device_handle_t dev = nullptr;
  uint8_t iface = 0, ep = 0;
  usb_transfer_t* in = nullptr;    // the interrupt IN transfer, always submitted while the keyboard is there
  usb_transfer_t* ctrl = nullptr;  // SET_PROTOCOL / SET_IDLE
  bool gone = false, in_busy = false;
  bool mouse = false;
};
static UsbKbd g_usbk[USB_KBD_MAX];
static uint8_t g_usb_new[8];       // addresses of new devices, for the client task (from its event callback)
static int g_usb_new_n = 0;
struct UsbStats { uint32_t keyboards, mice, reports, mouse_reports; };
static UsbStats g_ustats = {};

static void usb_in_done(usb_transfer_t* t) {
  UsbKbd& k = g_usbk[(intptr_t)t->context];
  if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
    if (k.mouse) {   // buttons, x, y (down is +, as the PS/2 mouse event takes it), maybe a wheel
      const uint8_t* r = t->data_buffer;
      if (t->actual_num_bytes >= 3 && g_pc && !g_panel_open)
        input_put({1, (int8_t)(r[0] & 7), (int8_t)r[1], (int8_t)r[2]});
      ++g_ustats.mouse_reports;
    } else if (t->actual_num_bytes >= 8) {
      UsbReport rep{(int8_t)(intptr_t)t->context, {}};
      memcpy(rep.r, t->data_buffer, 8);
      ++g_ustats.reports;
      xQueueSend(g_usb_q, &rep, 0);
    }
  } else if (t->status != USB_TRANSFER_STATUS_TIMED_OUT) {   // gone, cancelled, stalled: stop
    k.in_busy = false;
    return;
  }
  if (k.gone || usb_host_transfer_submit(t) != ESP_OK) k.in_busy = false;
}

static void usb_ctrl_done(usb_transfer_t*) {}

// HID class request to the keyboard's interface (no data stage)
static void usb_hid_request(UsbKbd& k, uint8_t request, uint16_t value) {
  usb_setup_packet_t* s = (usb_setup_packet_t*)k.ctrl->data_buffer;
  s->bmRequestType = 0x21;   // host to device, class, interface
  s->bRequest = request;
  s->wValue = value;
  s->wIndex = k.iface;
  s->wLength = 0;
  k.ctrl->num_bytes = sizeof(usb_setup_packet_t);
  k.ctrl->device_handle = k.dev;
  k.ctrl->bEndpointAddress = 0;
  k.ctrl->callback = usb_ctrl_done;
  usb_host_transfer_submit_control(g_usb_client, k.ctrl);
}

// What a device is, on the serial console: its IDs, name and interfaces (class / subclass / protocol).
static void usb_describe(uint8_t addr, usb_device_handle_t dev, const usb_config_desc_t* cfg) {
  const usb_device_desc_t* dd;
  usb_device_info_t info;
  char name[64] = "";
  if (usb_host_device_info(dev, &info) == ESP_OK && info.str_desc_product) {
    const usb_str_desc_t* sd = info.str_desc_product;
    int n = 0;
    for (int i = 0; i < (sd->bLength - 2) / 2 && n < (int)sizeof(name) - 1; ++i)
      name[n++] = sd->wData[i] < 128 ? (char)sd->wData[i] : '?';
    name[n] = 0;
  }
  if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK)
    Serial.printf("usb: device %u: %04x:%04x \"%s\"\n", addr, dd->idVendor, dd->idProduct, name);
  int off = 0;
  const usb_standard_desc_t* d = (const usb_standard_desc_t*)cfg;
  while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off))) {
    if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
      Serial.printf("usb:   interface %u.%u: class %u subclass %u protocol %u\n", i->bInterfaceNumber,
                    i->bAlternateSetting, i->bInterfaceClass, i->bInterfaceSubClass, i->bInterfaceProtocol);
    } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
      const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
      Serial.printf("usb:     endpoint %02x, type %u, %u bytes\n", e->bEndpointAddress, e->bmAttributes & 3,
                    USB_EP_DESC_GET_MPS(e));
    } else if (d->bDescriptorType == 0x21) {   // HID descriptor: its report descriptor's length
      const uint8_t* h = (const uint8_t*)d;
      Serial.printf("usb:     HID, report descriptor %u bytes\n", h[7] | (h[8] << 8));
    }
  }
}

// A new device: claim its boot keyboard and mouse interfaces, if it has them.
static void usb_open(uint8_t addr) {
  usb_device_handle_t dev;
  if (usb_host_device_open(g_usb_client, addr, &dev) != ESP_OK) return;
  const usb_config_desc_t* cfg;
  if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) { usb_host_device_close(g_usb_client, dev); return; }
  usb_describe(addr, dev, cfg);
  int off = 0;
  const usb_standard_desc_t* d = (const usb_standard_desc_t*)cfg;
  const usb_intf_desc_t* kbd_if = nullptr;
  bool claimed = false;
  while ((d = usb_parse_next_descriptor(d, cfg->wTotalLength, &off))) {
    if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
      const usb_intf_desc_t* i = (const usb_intf_desc_t*)d;
      kbd_if = i->bInterfaceClass == 3 && i->bInterfaceSubClass == 1 &&
               (i->bInterfaceProtocol == 1 || i->bInterfaceProtocol == 2) && i->bAlternateSetting == 0 ? i : nullptr;
    } else if (d->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT && kbd_if) {
      const usb_ep_desc_t* e = (const usb_ep_desc_t*)d;
      if (!(e->bEndpointAddress & 0x80) || (e->bmAttributes & 3) != USB_BM_ATTRIBUTES_XFER_INT) continue;
      int slot = 0;
      while (slot < USB_KBD_MAX && g_usbk[slot].dev) ++slot;
      if (slot == USB_KBD_MAX) break;
      if (usb_host_interface_claim(g_usb_client, dev, kbd_if->bInterfaceNumber, 0) != ESP_OK) break;
      UsbKbd& k = g_usbk[slot];
      k = UsbKbd{};
      k.dev = dev;
      k.iface = kbd_if->bInterfaceNumber;
      k.ep = e->bEndpointAddress;
      k.mouse = kbd_if->bInterfaceProtocol == 2;
      const int mps = USB_EP_DESC_GET_MPS(e);
      usb_host_transfer_alloc(sizeof(usb_setup_packet_t), 0, &k.ctrl);
      usb_host_transfer_alloc(mps, 0, &k.in);
      usb_hid_request(k, 0x0B, 0);   // SET_PROTOCOL: boot
      k.in->device_handle = dev;
      k.in->bEndpointAddress = k.ep;
      k.in->callback = usb_in_done;
      k.in->context = (void*)(intptr_t)slot;
      k.in->num_bytes = mps;
      k.in_busy = usb_host_transfer_submit(k.in) == ESP_OK;
      claimed = true;
      ++(k.mouse ? g_ustats.mice : g_ustats.keyboards);
      Serial.printf("usb: %s at address %u (interface %u)\n", k.mouse ? "mouse" : "keyboard", addr, k.iface);
      kbd_if = nullptr;   // the interface's first IN endpoint
    }
  }
  if (!claimed) usb_host_device_close(g_usb_client, dev);
}

static void usb_client_event(const usb_host_client_event_msg_t* m, void*) {
  if (m->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    if (g_usb_new_n < (int)sizeof(g_usb_new)) g_usb_new[g_usb_new_n++] = m->new_dev.address;
  } else if (m->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
    for (UsbKbd& k : g_usbk)
      if (k.dev == m->dev_gone.dev_hdl) k.gone = true;
  }
}

static void usb_client_task(void*) {
  usb_host_client_config_t cc = {};
  cc.is_synchronous = false;
  cc.max_num_event_msg = 8;
  cc.async.client_event_callback = usb_client_event;
  if (usb_host_client_register(&cc, &g_usb_client) != ESP_OK) { vTaskDelete(nullptr); return; }
  for (;;) {
    usb_host_client_handle_events(g_usb_client, pdMS_TO_TICKS(50));
    while (g_usb_new_n) usb_open(g_usb_new[--g_usb_new_n]);
    for (int i = 0; i < USB_KBD_MAX; ++i) {   // unplugged: stop its transfer, then let it go
      UsbKbd& k = g_usbk[i];
      if (!k.dev || !k.gone) continue;
      if (k.in_busy) {
        usb_host_endpoint_halt(k.dev, k.ep);
        usb_host_endpoint_flush(k.dev, k.ep);
        continue;   // its callback comes with the next events
      }
      usb_host_endpoint_clear(k.dev, k.ep);
      usb_host_interface_release(g_usb_client, k.dev, k.iface);
      bool shared = false;   // the device's other interface (a receiver's keyboard and mouse) still open
      for (int j = 0; j < USB_KBD_MAX; ++j) shared |= j != i && g_usbk[j].dev == k.dev;
      if (!shared) usb_host_device_close(g_usb_client, k.dev);
      usb_host_transfer_free(k.in);
      usb_host_transfer_free(k.ctrl);
      const bool mouse = k.mouse;
      k = UsbKbd{};
      if (mouse) {
        if (g_pc) input_put({1, 0, 0, 0});   // its buttons up
      } else {
        UsbReport rep{(int8_t)i, {}};   // its keys up
        xQueueSend(g_usb_q, &rep, portMAX_DELAY);
      }
      Serial.printf("usb: %s unplugged\n", mouse ? "mouse" : "keyboard");
    }
  }
}

static void usb_lib_task(void*) {
  usb_host_config_t hc = {};
  hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
  if (usb_host_install(&hc) != ESP_OK) {
    Serial.println("usb: host library failed to start");
    vTaskDelete(nullptr);
    return;
  }
  xTaskCreatePinnedToCore(usb_client_task, "usb_kbd", 4096, nullptr, 3, nullptr, 0);
  for (;;) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
  }
}

static void usb_kbd_setup() {
  g_usb_q = xQueueCreate(32, sizeof(UsbReport));
  xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
  M5.Power.setExtOutput(true, m5::ext_port_mask_t::ext_USB);   // 5 V on the USB-A port (off after power-up)
  xSemaphoreGive(g_i2c_lock);
  xTaskCreatePinnedToCore(usb_lib_task, "usb_host", 4096, nullptr, 4, nullptr, 0);
}

// ---- the display task's side --------------------------------------------------------------------------------

// Linux key codes (ps2_put_keycode's) of the HID usage codes past keyboard.h's table: the keypad, the extra key of
// ISO keyboards and the menu key (0x53-0x65), and the modifier bits (0xE0-0xE7 as bits 0-7).
static const uint8_t USB_HID_TO_LINUX_HI[0x66 - 0x53] = {
  69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71, 72, 73, 82, 83, 86, 127,
};
static const uint8_t USB_MOD_TO_LINUX[8] = {29, 42, 56, 125, 97, 54, 100, 126};   // L Ctrl Shift Alt GUI, R ...

static int usb_linux_code(uint8_t hid) {
  if (hid < sizeof(HID_TO_LINUX)) return HID_TO_LINUX[hid];
  if (hid < 0x66) return USB_HID_TO_LINUX_HI[hid - 0x53];
  return 0;
}

static uint8_t g_usb_held[USB_KBD_MAX][8];   // the last report of each keyboard, as the PC knows it

static bool usb_has(const uint8_t* r, uint8_t code) {
  for (int i = 2; i < 8; ++i)
    if (r[i] == code) return true;
  return false;
}

// The PC lets go of everything held on USB keyboards (the settings panel opens).
static void usb_kbd_release_all() {
  for (auto& held : g_usb_held) {
    for (int b = 0; b < 8; ++b)
      if (held[0] & (1 << b)) pc_key(false, USB_MOD_TO_LINUX[b]);
    for (int i = 2; i < 8; ++i)
      if (held[i] > 3) pc_key(false, usb_linux_code(held[i]));
    memset(held, 0, 8);
  }
}

static void usb_kbd_poll() {
  if (!g_usb_q) return;
  UsbReport rep;
  while (xQueueReceive(g_usb_q, &rep, 0) == pdTRUE) {
    uint8_t* held = g_usb_held[rep.slot];
    const uint8_t* r = rep.r;
    if (r[2] == 1) continue;   // ErrorRollOver: too many keys at once, nothing to go by
    ++g_kbd.keys;
    if (usb_has(r, HID_SCROLL_LOCK) && !usb_has(held, HID_SCROLL_LOCK)) {   // the settings panel
      usb_kbd_release_all();
      memcpy(held, r, 8);
      panel_toggle();
      continue;
    }
    if (g_panel_open) {   // typing into the panel: new keys only
      for (int i = 2; i < 8; ++i)
        if (r[i] > 3 && !usb_has(held, r[i])) kbd_type_in_panel({r[i], (uint8_t)(r[0] & 0x22 ? KMOD_SHIFT : 0)});
      memcpy(held, r, 8);
      continue;
    }
    for (int b = 0; b < 8; ++b)   // modifiers first: Shift goes down before the key it shifts
      if ((r[0] ^ held[0]) & (1 << b)) pc_key(r[0] & (1 << b), USB_MOD_TO_LINUX[b]);
    for (int i = 2; i < 8; ++i)
      if (held[i] > 3 && !usb_has(r, held[i])) pc_key(false, usb_linux_code(held[i]));
    for (int i = 2; i < 8; ++i)
      if (r[i] > 3 && r[i] != HID_SCROLL_LOCK && !usb_has(held, r[i])) pc_key(true, usb_linux_code(r[i]));
    memcpy(held, r, 8);
  }
}
