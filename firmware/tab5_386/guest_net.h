// The emulated PC's network, like a home router's: its NE2000 is linked to a network of its own inside the Tab5
// (192.168.77.0/24, the Tab5 at .1 with a DHCP server and a DNS relay), routed to the Wi-Fi network with address
// translation (lwIP's NAPT, in the Arduino core's build). Ports 2222, 2323 and 8080 of the Tab5 lead to the guest's
// 22, 23 and 80. It is there from the start, Wi-Fi on or off: with Wi-Fi off, the PC still gets its address (a
// DHCP client finding no server held up some systems' start with a message, and Linux's boot for a while), and
// reaches the Wi-Fi network once it is on. The DNS server the PC is given is the Tab5 (.1), which relays its
// queries to the Wi-Fi network's DNS server of the moment (the lease stays right when Wi-Fi comes on later, or
// joins another network).
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
#include "lwip/udp.h"
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
static bool g_gnet_up;                    // the netif exists (from the start, guest_net_start)
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

// ---- the DNS relay (lwIP's thread) ----
// A query to .1:53 goes to the Wi-Fi network's DNS server under an ID of our own; its answer back to the guest with
// the guest's ID. Without Wi-Fi (or a DNS server), "server failure" at once, so lookups fail fast offline.
static struct udp_pcb *g_gdns_in, *g_gdns_out;
static struct { uint16_t id, guest_id, port; ip_addr_t addr; } g_gdns_q[16];   // queries out, the oldest replaced
static unsigned g_gdns_next;
static uint16_t g_gdns_id;
static std::atomic<uint32_t> g_gdns_relayed, g_gdns_failed;

static void gdns_fail(struct pbuf* p, const ip_addr_t* addr, u16_t port) {   // the query back as SERVFAIL
  uint8_t h[4];
  if (pbuf_copy_partial(p, h, 4, 0) == 4) {
    h[2] = (h[2] & 0x79) | 0x80;   // QR, the opcode and RD kept (AA, TC cleared)
    h[3] = 0x80 | 2;               // RA, rcode 2
    pbuf_take_at(p, h + 2, 2, 2);
    udp_sendto(g_gdns_in, p, addr, port);
  }
  g_gdns_failed++;
}

static void gdns_query(void*, struct udp_pcb*, struct pbuf* p, const ip_addr_t* addr, u16_t port) {
  const struct netif* sta = netif_default;
  const ip_addr_t* server = dns_getserver(0);
  uint8_t id[2];
  if (p->tot_len < 12 || pbuf_copy_partial(p, id, 2, 0) != 2) { pbuf_free(p); return; }
  if (!sta || sta == &g_gnet_if || !netif_is_up(sta) || !netif_is_link_up(sta) ||
      ip4_addr_isany_val(*netif_ip4_addr(sta)) || !server || ip_addr_isany(server)) {
    gdns_fail(p, addr, port);
    pbuf_free(p);
    return;
  }
  auto& q = g_gdns_q[g_gdns_next++ % (sizeof g_gdns_q / sizeof g_gdns_q[0])];
  q.id = ++g_gdns_id;
  q.guest_id = id[0] << 8 | id[1];
  q.port = port;
  ip_addr_copy(q.addr, *addr);
  const uint8_t ours[2] = {(uint8_t)(q.id >> 8), (uint8_t)q.id};
  pbuf_take_at(p, ours, 2, 0);
  if (udp_sendto(g_gdns_out, p, server, 53) != ERR_OK) {
    pbuf_take_at(p, id, 2, 0);
    q.port = 0;
    gdns_fail(p, addr, port);
  } else {
    g_gdns_relayed++;
  }
  pbuf_free(p);
}

static void gdns_answer(void*, struct udp_pcb*, struct pbuf* p, const ip_addr_t*, u16_t) {
  uint8_t id[2];
  if (p->tot_len >= 12 && pbuf_copy_partial(p, id, 2, 0) == 2) {
    const uint16_t ours = id[0] << 8 | id[1];
    for (auto& q : g_gdns_q) {
      if (q.port && q.id == ours) {
        const uint8_t guest[2] = {(uint8_t)(q.guest_id >> 8), (uint8_t)q.guest_id};
        pbuf_take_at(p, guest, 2, 0);
        udp_sendto(g_gdns_in, p, &q.addr, q.port);
        q.port = 0;
        break;
      }
    }
  }
  pbuf_free(p);
}

static bool gdns_start(const ip4_addr_t& tab5) {
  g_gdns_in = udp_new();
  g_gdns_out = udp_new();
  if (!g_gdns_in || !g_gdns_out) return false;
  ip_addr_t a;
  ip_addr_copy_from_ip4(a, tab5);
  if (udp_bind(g_gdns_in, &a, 53) != ERR_OK || udp_bind(g_gdns_out, IP_ADDR_ANY, 0) != ERR_OK) return false;
  udp_recv(g_gdns_in, gdns_query, nullptr);
  udp_recv(g_gdns_out, gdns_answer, nullptr);
  return true;
}

static struct { SemaphoreHandle_t done; bool ok; } g_gnet_setup;

static void gnet_setup_cb(void*) {   // (lwIP's thread)
  ip4_addr_t ip, mask, gw;
  IP4_ADDR(&ip, GNET_NET[0], GNET_NET[1], GNET_NET[2], 1);
  IP4_ADDR(&mask, 255, 255, 255, 0);
  gw = ip;   // (the DHCP server offers the netif's gateway as the router: the Tab5 itself, as on a soft AP)
  g_gnet_setup.ok = false;
  if (netif_add(&g_gnet_if, &ip, &mask, &gw, nullptr, gnet_netif_init, tcpip_input)) {
    netif_set_up(&g_gnet_if);
    netif_set_link_up(&g_gnet_if);
    ip_napt_enable_netif(&g_gnet_if, 1);
    g_gnet_dhcps = dhcps_new();
    if (g_gnet_dhcps && gdns_start(ip)) {
      dhcps_offer_t router = OFFER_ROUTER, dns = OFFER_DNS;
      dhcps_set_option_info(g_gnet_dhcps, ROUTER_SOLICITATION_ADDRESS, &router, sizeof router);
      dhcps_set_option_info(g_gnet_dhcps, DOMAIN_NAME_SERVER, &dns, sizeof dns);
      dhcps_set_new_lease_cb(g_gnet_dhcps, gnet_lease, nullptr);
      ip_addr_t relay;
      ip_addr_copy_from_ip4(relay, ip);
      dhcps_dns_setserver(g_gnet_dhcps, &relay);   // (the relay above)
      g_gnet_setup.ok = dhcps_start(g_gnet_dhcps, &g_gnet_if, ip) == ERR_OK;
    }
  }
  xSemaphoreGive(g_gnet_setup.done);
}

// At the start (setup), before the PC runs: lwIP (its thread; Wi-Fi's start finds it there) and the guest's network
static void guest_net_start() {
  if (g_gnet_up) return;
  if (!Network.begin()) { Serial.println("net: lwIP did not start: the PC has no network"); return; }
  g_gnet_rx = xQueueCreate(64, sizeof(GnetFrame*));
  g_gnet_setup.done = xSemaphoreCreateBinary();
  const bool called = g_gnet_rx && g_gnet_setup.done && tcpip_callback(gnet_setup_cb, nullptr) == ERR_OK;
  if (called) xSemaphoreTake(g_gnet_setup.done, portMAX_DELAY);
  if (g_gnet_setup.done) vSemaphoreDelete(g_gnet_setup.done);
  if (!called || !g_gnet_setup.ok) {   // (the card then sends into nothing, as with no network)
    Serial.println("net: the PC's network could not be set up");
    return;
  }
  g_gnet_up = true;
  t386_ne2000_set_sender(gnet_send);
  Serial.printf("net: the PC's network %u.%u.%u.0/24 (routed to Wi-Fi while it is on)\n", GNET_NET[0], GNET_NET[1],
                GNET_NET[2]);
}

// Wi-Fi joined a network (net.h): the port forwards on the new address
static void guest_net_wifi_up() {
  if (!g_gnet_up) return;
  tcpip_callback([](void*) { gnet_update_forwards(); }, nullptr);
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
                "queued %u, announced again %lu | DNS relayed %lu, failed %lu | card: %s | forwards 2222->22 "
                "2323->23 8080->80\n",
                g_gnet_up ? (g_wifi == WIFI_UP ? "up, routed to Wi-Fi" : "up, Wi-Fi not connected") : "off",
                g_gnet_guest_mac[0], g_gnet_guest_mac[1],
                g_gnet_guest_mac[2], g_gnet_guest_mac[3], g_gnet_guest_mac[4], g_gnet_guest_mac[5], guest,
                (unsigned long)g_gnet_tx, (unsigned long)g_gnet_rx_n, (unsigned long)g_gnet_drops,
                g_gnet_rx ? (unsigned)uxQueueMessagesWaiting(g_gnet_rx) : 0u, (unsigned long)g_gnet_kicks,
                (unsigned long)g_gdns_relayed, (unsigned long)g_gdns_failed, card);
}
