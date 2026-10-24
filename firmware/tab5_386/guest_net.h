// The emulated PC's network, like a home router's: its NE2000 is linked to a network of its own inside the Tab5
// (192.168.77.0/24, the Tab5 at .1 with a DHCP server that also hands out the Wi-Fi network's DNS server), routed to
// the Wi-Fi network with address translation (lwIP's NAPT, in the Arduino core's build). Ports 2222, 2323 and 8080
// of the Tab5 lead to the guest's 22, 23 and 80. It exists while Wi-Fi is on (lwIP's thread starts with Wi-Fi).
//
// Frames from the guest come on core 1 (tiny386's esp32_send_packet) and go straight into lwIP's thread; frames for
// it are queued (PSRAM) and handed to the card by core 1 between the PC's steps (guest_net_deliver), so tiny386
// itself is only ever touched from its own core.
#pragma once
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "lwip/dns.h"
#include "lwip/lwip_napt.h"
#include "lwip/prot/ip.h"
#include "dhcpserver/dhcpserver.h"
#include "dhcpserver/dhcpserver_options.h"
#include "esp_mac.h"

extern "C" {
void t386_ne2000_set_mac(const uint8_t mac[6]);
int t386_ne2000_receive(const uint8_t* buf, int size);
void t386_ne2000_set_sender(void (*send)(uint8_t* buf, int size));
void t386_ne2000_kick(void);
int t386_ne2000_state(char* buf, int len);
}

static constexpr uint8_t GNET_NET[3] = {192, 168, 77};   // the Tab5 is .1
static const struct { uint16_t tab5, guest; } GNET_FORWARDS[] = {{2222, 22}, {2323, 23}, {8080, 80}};

static struct netif g_gnet_if;
static bool g_gnet_up;                    // the netif exists (from the first time Wi-Fi is on)
static dhcps_t* g_gnet_dhcps;
static QueueHandle_t g_gnet_rx;           // frames for the guest: GnetFrame*
static uint8_t g_gnet_mac[6], g_gnet_guest_mac[6];
static uint32_t g_gnet_guest_ip, g_gnet_mapped_tab5_ip, g_gnet_mapped_guest_ip;   // (network order)
static std::atomic<uint32_t> g_gnet_tx, g_gnet_rx_n, g_gnet_drops, g_gnet_kicks;
struct GnetFrame {
  uint16_t len;
  uint8_t data[];
};

// Before the guest starts: its card's MAC address, a locally administered one made from the Tab5's (the Tab5's
// side of the link: the same with another first byte)
static void guest_net_mac() {
  uint8_t m[6] = {0};
  esp_efuse_mac_get_default(m);
  const uint8_t guest[6] = {0x02, 0x54, 0x35, m[3], m[4], m[5]};
  memcpy(g_gnet_guest_mac, guest, 6);
  memcpy(g_gnet_mac, guest, 6);
  g_gnet_mac[0] = 0x06;
  t386_ne2000_set_mac(g_gnet_guest_mac);
}

// lwIP's thread: a frame for the guest
static err_t gnet_linkoutput(struct netif* n, struct pbuf* p) {
  GnetFrame* f = (GnetFrame*)heap_caps_malloc(sizeof(GnetFrame) + p->tot_len, MALLOC_CAP_SPIRAM);
  if (!f) { g_gnet_drops++; return ERR_MEM; }
  f->len = pbuf_copy_partial(p, f->data, p->tot_len, 0);
  if (xQueueSend(g_gnet_rx, &f, 0) != pdTRUE) {
    heap_caps_free(f);
    g_gnet_drops++;
  }
  return ERR_OK;
}

// Core 1, from tiny386's NE2000: a frame from the guest
static void gnet_send(uint8_t* buf, int size) {
  if (size <= 0 || size > 1514) return;
  struct pbuf* p = pbuf_alloc(PBUF_RAW, size, PBUF_RAM);
  if (!p) { g_gnet_drops++; return; }
  pbuf_take(p, buf, size);
  if (g_gnet_if.input(p, &g_gnet_if) != ERR_OK) {   // (tcpip_input: into lwIP's thread's mailbox)
    pbuf_free(p);
    g_gnet_drops++;
    return;
  }
  g_gnet_tx++;
}

// Core 1, between the PC's steps: hand queued frames to the card while it has room. A ring full for 50 ms gets its
// frames announced again (t386_ne2000_kick: the guest may have missed one).
static void guest_net_deliver() {
  static int64_t full_since = 0;
  GnetFrame* f;
  while (xQueuePeek(g_gnet_rx, &f, 0) == pdTRUE) {
    if (!t386_ne2000_receive(f->data, f->len)) {   // its ring is full: later
      const int64_t now = esp_timer_get_time();
      if (!full_since) full_since = now;
      else if (now - full_since > 50000) {
        t386_ne2000_kick();
        g_gnet_kicks++;
        full_since = now;
      }
      return;
    }
    full_since = 0;
    xQueueReceive(g_gnet_rx, &f, 0);
    heap_caps_free(f);
    g_gnet_rx_n++;
  }
}

static err_t gnet_netif_init(struct netif* n) {
  n->name[0] = 'g';
  n->name[1] = 'n';
  n->output = etharp_output;
  n->linkoutput = gnet_linkoutput;
  n->mtu = 1500;
  n->hwaddr_len = 6;
  memcpy(n->hwaddr, g_gnet_mac, 6);
  n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
  return ERR_OK;
}

// lwIP's thread: the port forwards to the guest's address on the Tab5's Wi-Fi address (both can change)
static void gnet_update_forwards() {
  const struct netif* sta = netif_default;
  const uint32_t tab5 = sta && sta != &g_gnet_if ? ip4_addr_get_u32(netif_ip4_addr(sta)) : 0;
  if (tab5 == g_gnet_mapped_tab5_ip && g_gnet_guest_ip == g_gnet_mapped_guest_ip) return;
  for (auto& f : GNET_FORWARDS) ip_portmap_remove(IP_PROTO_TCP, f.tab5);
  if (tab5 && g_gnet_guest_ip)
    for (auto& f : GNET_FORWARDS) ip_portmap_add(IP_PROTO_TCP, tab5, f.tab5, g_gnet_guest_ip, f.guest);
  g_gnet_mapped_tab5_ip = tab5;
  g_gnet_mapped_guest_ip = g_gnet_guest_ip;
}

static void gnet_lease(void* arg, u8_t ip[4], u8_t mac[6]) {   // (lwIP's thread)
  if (memcmp(mac, g_gnet_guest_mac, 6) == 0) {
    memcpy(&g_gnet_guest_ip, ip, 4);
    gnet_update_forwards();
  }
}

static void gnet_set_dns() {   // (lwIP's thread) the Wi-Fi network's DNS server, for the guest's leases
  const ip_addr_t* d = dns_getserver(0);
  if (d && !ip_addr_isany(d)) dhcps_dns_setserver(g_gnet_dhcps, d);
}

static void gnet_setup_cb(void* done) {   // (lwIP's thread)
  ip4_addr_t ip, mask, gw;
  IP4_ADDR(&ip, GNET_NET[0], GNET_NET[1], GNET_NET[2], 1);
  IP4_ADDR(&mask, 255, 255, 255, 0);
  gw = ip;   // (the DHCP server offers the netif's gateway as the router: the Tab5 itself, as on a soft AP)
  netif_add(&g_gnet_if, &ip, &mask, &gw, nullptr, gnet_netif_init, tcpip_input);
  netif_set_up(&g_gnet_if);
  netif_set_link_up(&g_gnet_if);
  ip_napt_enable_netif(&g_gnet_if, 1);
  g_gnet_dhcps = dhcps_new();
  dhcps_offer_t router = OFFER_ROUTER, dns = OFFER_DNS;
  dhcps_set_option_info(g_gnet_dhcps, ROUTER_SOLICITATION_ADDRESS, &router, sizeof router);
  dhcps_set_option_info(g_gnet_dhcps, DOMAIN_NAME_SERVER, &dns, sizeof dns);
  dhcps_set_new_lease_cb(g_gnet_dhcps, gnet_lease, nullptr);
  gnet_set_dns();
  dhcps_start(g_gnet_dhcps, &g_gnet_if, ip);
  xSemaphoreGive((SemaphoreHandle_t)done);
}

// Once Wi-Fi's stack is up (net.h): the guest's network
static void guest_net_start() {
  if (g_gnet_up) return;
  g_gnet_rx = xQueueCreate(64, sizeof(GnetFrame*));
  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (tcpip_callback(gnet_setup_cb, done) == ERR_OK) xSemaphoreTake(done, portMAX_DELAY);
  vSemaphoreDelete(done);
  g_gnet_up = true;
  t386_ne2000_set_sender(gnet_send);
  Serial.printf("net: the PC's network %u.%u.%u.0/24, routed to Wi-Fi\n", GNET_NET[0], GNET_NET[1], GNET_NET[2]);
}

// Wi-Fi joined a network (net.h): its DNS server for the guest, the port forwards on the new address
static void guest_net_wifi_up() {
  if (!g_gnet_up) return;
  tcpip_callback([](void*) { gnet_set_dns(); gnet_update_forwards(); }, nullptr);
}

static void guest_net_report() {   // serial "net"
  char guest[16] = "none";
  if (g_gnet_guest_ip) {
    const uint8_t* b = (const uint8_t*)&g_gnet_guest_ip;
    snprintf(guest, sizeof guest, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
  }
  char card[96];
  t386_ne2000_state(card, sizeof card);   // (read from core 0: only a snapshot)
  Serial.printf("net: %s | guest %02x:%02x:%02x:%02x:%02x:%02x at %s | frames from it %lu, to it %lu, dropped %lu, "
                "queued %u, announced again %lu | card: %s | forwards 2222->22 2323->23 8080->80\n",
                g_gnet_up ? "up" : "off (Wi-Fi hasn't been on)", g_gnet_guest_mac[0], g_gnet_guest_mac[1],
                g_gnet_guest_mac[2], g_gnet_guest_mac[3], g_gnet_guest_mac[4], g_gnet_guest_mac[5], guest,
                (unsigned long)g_gnet_tx, (unsigned long)g_gnet_rx_n, (unsigned long)g_gnet_drops,
                g_gnet_rx ? (unsigned)uxQueueMessagesWaiting(g_gnet_rx) : 0u, (unsigned long)g_gnet_kicks, card);
}
