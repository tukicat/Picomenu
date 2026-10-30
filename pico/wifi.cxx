/*
This file is part of picoFace.

picoFace is free software: you can redistribute it and/or modify it under the terms of
the GNU General Public License as published by the Free Software Foundation, either
version 3 of the License, or (at your option) any later version.

picoFace is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with picoFace. If
not, see <https://www.gnu.org/licenses/>.
*/

// The Pico W's WiFi for the file transfer (wifi.h). Only in the Pico W build.
//
// Memory: lwIP is built with its heap at picomenu_lwip_heap (lwipopts.h: LWIP_RAM_HEAP_POINTER,
// MEMP_MEM_MALLOC) and the connections are placed at the start of the arena, so nothing of it
// is in the program's own RAM: the arena is lent by PicoMenu while F - File transfer is open,
// and cyw43_arch_init (which starts lwIP) is only called then, cyw43_arch_deinit when it closes.
//
// Each browser connection is a web::Conn: what lwIP receives is fed to it, and what it has
// ready is written as there is room in lwIP's send buffer (on each poll, and as data is acked).
// It is closed once its reply has gone.

#include <new>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/igmp.h"
#include "lwip/apps/netbiosns.h"
#include "wifi.h"
#include "bt.h"

extern "C" {
unsigned char* picomenu_lwip_heap = nullptr;        // lwIP's heap (lwipopts.h)
}

namespace wifi {

namespace {

const int CONNS = 3;
const uint32_t JOIN_TIMEOUT_MS = 30000;

struct Slot {
    web::Conn conn;
    struct tcp_pcb* pcb;
    bool used;
};

Slot* slots = nullptr;
struct tcp_pcb* listener = nullptr;
State st = OFF;
bool started = false;               // cyw43_arch_init done
bool ever = false;                  // lwIP has been started before (its state is stale now)
uint32_t join_began = 0;
char addr[20];
const char* reason = "";
const web::Hooks* hooks_ = nullptr;
const char* web_password_ = "";

void slot_free(Slot* s) {
    if (s == nullptr) return;
    s->used = false;
    s->pcb = nullptr;
}

void slot_close(Slot* s) {
    if (s->pcb != nullptr) {
        struct tcp_pcb* pcb = s->pcb;
        tcp_arg(pcb, nullptr);
        tcp_recv(pcb, nullptr);
        tcp_sent(pcb, nullptr);
        tcp_err(pcb, nullptr);
        tcp_poll(pcb, nullptr, 0);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
    slot_free(s);
}

// As much of the reply as there is room for; closes the connection when it has all gone
void send_more(Slot* s) {
    if (s->pcb == nullptr) return;
    for (;;) {
        const uint8_t* p;
        size_t n = s->conn.ready(&p);
        if (n == 0) break;
        size_t room = tcp_sndbuf(s->pcb);
        if (room == 0) break;
        size_t take = n < room ? n : room;
        if (take > 0xFFFF) take = 0xFFFF;
        if (tcp_write(s->pcb, p, (u16_t) take, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
        s->conn.sent(take);
    }
    tcp_output(s->pcb);
    if (s->conn.finished() && s->pcb->unsent == nullptr && s->pcb->unacked == nullptr) {
        slot_close(s);
    }
}

err_t on_recv(void* arg, struct tcp_pcb* pcb, struct pbuf* p, err_t err) {
    Slot* s = static_cast<Slot*>(arg);
    if (s == nullptr) {
        if (p != nullptr) pbuf_free(p);
        return ERR_OK;
    }
    if (p == nullptr) {
        // the browser closed its side: anything not finished (an upload cut short) goes
        if (!s->conn.finished()) s->conn.abort();
        slot_close(s);
        return ERR_OK;
    }
    for (struct pbuf* q = p; q != nullptr; q = q->next) {
        s->conn.feed(static_cast<const uint8_t*>(q->payload), q->len);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    send_more(s);
    return ERR_OK;
}

err_t on_sent(void* arg, struct tcp_pcb*, u16_t) {
    Slot* s = static_cast<Slot*>(arg);
    if (s != nullptr) send_more(s);
    return ERR_OK;
}

err_t on_poll(void* arg, struct tcp_pcb*) {
    Slot* s = static_cast<Slot*>(arg);
    if (s != nullptr) send_more(s);
    return ERR_OK;
}

void on_err(void* arg, err_t) {
    Slot* s = static_cast<Slot*>(arg);           // the pcb is gone already
    if (s == nullptr) return;
    s->conn.abort();
    slot_free(s);
}

err_t on_accept(void*, struct tcp_pcb* pcb, err_t err) {
    if (err != ERR_OK || pcb == nullptr) return ERR_VAL;
    Slot* s = nullptr;
    for (int i = 0; i < CONNS; i++) {
        if (!slots[i].used) {
            s = &slots[i];
            break;
        }
    }
    if (s == nullptr) {
        tcp_abort(pcb);                         // too many at once: the browser tries again
        return ERR_ABRT;
    }
    s->used = true;
    s->pcb = pcb;
    s->conn.begin(hooks_, web_password_);
    tcp_arg(pcb, s);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 1);
    return ERR_OK;
}

// ---- The name: http://picomenu.local (mDNS: Windows 10 and later, macOS, iOS, Linux with
// Avahi, newer Android) and http://picomenu/ (NetBIOS: Windows). A small mDNS responder of our
// own (lwIP's keeps state across starts that does not survive lwIP being started afresh): it
// answers questions for picomenu.local's address (type A or ANY), and says it once on joining.
const char MDNS_LABEL[] = "picomenu";
struct udp_pcb* mdns = nullptr;
ip4_addr_t mdns_group;

bool name_is_ours(const uint8_t* d, size_t n, size_t& at) {
    // the question's name, at d[at], as labels (a pointer followed once): picomenu.local?
    static const char* const WANT[] = { MDNS_LABEL, "local" };
    size_t i = at;
    bool jumped = false;
    int label = 0;
    bool same = true;
    for (int guard = 0; guard < 20; guard++) {
        if (i >= n) return false;
        uint8_t len = d[i];
        if ((len & 0xC0) == 0xC0) {
            if (i + 1 >= n) return false;
            if (!jumped) at = i + 2;
            jumped = true;
            i = ((size_t) (len & 0x3F) << 8) | d[i + 1];
            continue;
        }
        if (len == 0) {
            if (!jumped) at = i + 1;
            return same && label == 2;
        }
        if (i + 1 + len > n) return false;
        if (label >= 2 || strlen(WANT[label]) != len || strncasecmp((const char*) d + i + 1, WANT[label], len) != 0) same = false;
        label++;
        i += 1 + len;
    }
    return false;
}

// The answer: picomenu.local is the address. To the group (port 5353), or to the one who asked
// from another port (a "legacy" question: then with its id and question)
void mdns_answer(const ip_addr_t* to, u16_t port, uint16_t id, bool legacy) {
    struct netif* nif = &cyw43_state.netif[CYW43_ITF_STA];
    uint8_t b[80];
    size_t n = 0;
    auto put16 = [&](uint16_t v) { b[n++] = (uint8_t) (v >> 8); b[n++] = (uint8_t) v; };
    auto name = [&]() {
        b[n++] = (uint8_t) strlen(MDNS_LABEL);
        memcpy(b + n, MDNS_LABEL, strlen(MDNS_LABEL));
        n += strlen(MDNS_LABEL);
        b[n++] = 5;
        memcpy(b + n, "local", 5);
        n += 5;
        b[n++] = 0;
    };
    put16(legacy ? id : 0);
    put16(0x8400);                  // a response, authoritative
    put16(legacy ? 1 : 0);          // questions
    put16(1);                       // answers
    put16(0);
    put16(0);
    if (legacy) {
        name();
        put16(1);                   // A
        put16(1);                   // IN
    }
    name();
    put16(1);                       // A
    put16(legacy ? 0x0001 : 0x8001);    // IN (and "flush the cache" to the group)
    put16(0);
    put16(120);                     // seconds it may be kept
    put16(4);
    const ip4_addr_t* ip = netif_ip4_addr(nif);
    memcpy(b + n, &ip->addr, 4);    // (network order already)
    n += 4;
    struct pbuf* p = pbuf_alloc(PBUF_TRANSPORT, (u16_t) n, PBUF_RAM);
    if (p == nullptr) return;
    memcpy(p->payload, b, n);
    udp_sendto_if(mdns, p, to, port, nif);
    pbuf_free(p);
}

void mdns_recv(void*, struct udp_pcb*, struct pbuf* p, const ip_addr_t* addr, u16_t port) {
    uint8_t d[300];
    size_t n = pbuf_copy_partial(p, d, sizeof(d), 0);
    pbuf_free(p);
    if (n < 12 || (d[2] & 0x80) != 0) return;           // not a question
    uint16_t id = (uint16_t) ((d[0] << 8) | d[1]);
    int questions = (d[4] << 8) | d[5];
    size_t at = 12;
    for (int q = 0; q < questions && q < 8; q++) {
        bool ours = name_is_ours(d, n, at);
        if (at + 4 > n) return;
        uint16_t type = (uint16_t) ((d[at] << 8) | d[at + 1]);
        at += 4;
        if (ours && (type == 1 || type == 255)) {
            if (port != 5353) {
                mdns_answer(addr, port, id, true);
            } else {
                ip_addr_t group;
                ip_addr_copy_from_ip4(group, mdns_group);
                mdns_answer(&group, 5353, 0, false);
            }
            return;
        }
    }
}

void names_start() {
    struct netif* nif = &cyw43_state.netif[CYW43_ITF_STA];
    IP4_ADDR(&mdns_group, 224, 0, 0, 251);
    mdns = udp_new_ip_type(IPADDR_TYPE_V4);
    if (mdns != nullptr) {
        mdns->ttl = 255;
        if (udp_bind(mdns, IP_ANY_TYPE, 5353) == ERR_OK) {
            igmp_joingroup_netif(nif, &mdns_group);
            udp_recv(mdns, mdns_recv, nullptr);
            ip_addr_t group;
            ip_addr_copy_from_ip4(group, mdns_group);
            mdns_answer(&group, 5353, 0, false);        // said once on joining
        } else {
            udp_remove(mdns);
            mdns = nullptr;
        }
    }
    netbiosns_set_name(MDNS_LABEL);
    netbiosns_init();
}

void names_stop() {
    netbiosns_stop();
    if (mdns != nullptr) {
        igmp_leavegroup_netif(&cyw43_state.netif[CYW43_ITF_STA], &mdns_group);
        udp_remove(mdns);
        mdns = nullptr;
    }
}

bool listen_http() {
    struct tcp_pcb* pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == nullptr) return false;
    if (tcp_bind(pcb, nullptr, 80) != ERR_OK) {
        tcp_abort(pcb);
        return false;
    }
    listener = tcp_listen_with_backlog(pcb, CONNS);
    if (listener == nullptr) {
        tcp_abort(pcb);
        return false;
    }
    tcp_accept(listener, on_accept);
    return true;
}

// lwIP's memory is lent (picomenu_lwip_heap set): if lwIP was started before (the SDK starts it
// only once, with the chip, for the WiFi or the Bluetooth) its memory has been used for other
// things since, so everything it keeps is forgotten and it is started afresh (stop() left no
// connections, and no timer but the cyclic ones). Without this a second start found lwIP's
// timers and heap where something else had been, and lwIP stopped the Pico (panic).
void lwip_fresh() {
    if (ever) {
        *sys_timeouts_get_next_timeout() = nullptr;
        netif_list = nullptr;
        netif_default = nullptr;
        tcp_active_pcbs = nullptr;
        tcp_tw_pcbs = nullptr;
        tcp_bound_pcbs = nullptr;
        tcp_listen_pcbs.listen_pcbs = nullptr;
        udp_pcbs = nullptr;
        lwip_init();
    }
    ever = true;
}

}   // namespace

const char* start(const char* ssid, const char* password, const char* web_password,
        uint8_t* arena, size_t size, const web::Hooks* hooks) {
    if (started) stop();
    size_t slots_size = (sizeof(Slot) * CONNS + 7) & ~(size_t) 7;
    if (size < slots_size + MEM_SIZE + 64) return "Not enough memory";
    slots = reinterpret_cast<Slot*>(arena);
    for (int i = 0; i < CONNS; i++) {
        new (&slots[i]) Slot();
        slots[i].used = false;
        slots[i].pcb = nullptr;
    }
    picomenu_lwip_heap = arena + slots_size;
    lwip_fresh();
    hooks_ = hooks;
    web_password_ = web_password;
    addr[0] = '\0';
    reason = "";
    bt::radio_clear();          // BTstack is started with the chip (its memory: bt.h)
    if (cyw43_arch_init() != 0) {
        st = FAILED;
        reason = "The WiFi chip did not start";
        return reason;
    }
    started = true;
    cyw43_arch_enable_sta_mode();
    uint32_t auth = password[0] == '\0' ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_MIXED_PSK;
    if (cyw43_arch_wifi_connect_async(ssid, password[0] == '\0' ? nullptr : password, auth) != 0) {
        st = FAILED;
        reason = "Could not start joining";
        return reason;
    }
    st = JOINING;
    join_began = to_ms_since_boot(get_absolute_time());
    return nullptr;
}

void poll() {
    if (!started) return;
    cyw43_arch_poll();
    if (st == JOINING) {
        int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (link == CYW43_LINK_UP) {
            const ip4_addr_t* ip = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
            snprintf(addr, sizeof(addr), "%s", ip4addr_ntoa(ip));
            if (listen_http()) {
                st = UP;
                names_start();
            } else {
                st = FAILED;
                reason = "Could not start the web page";
            }
        } else if (link == CYW43_LINK_BADAUTH) {
            st = FAILED;
            reason = "Wrong password";
        } else if (link == CYW43_LINK_NONET) {
            st = FAILED;
            reason = "Network not found";
        } else if (link == CYW43_LINK_FAIL) {
            st = FAILED;
            reason = "Could not join";
        } else if (to_ms_since_boot(get_absolute_time()) - join_began > JOIN_TIMEOUT_MS) {
            st = FAILED;
            reason = link == CYW43_LINK_NOIP ? "No address given (DHCP)" : "Timed out";
        }
    } else if (st == UP) {
        for (int i = 0; i < CONNS; i++) {
            if (slots[i].used) send_more(&slots[i]);    // (a file being read off the card)
        }
    }
}

void stop() {
    if (!started) {
        st = OFF;
        return;
    }
    for (int i = 0; i < CONNS; i++) {
        if (slots[i].used) {
            slots[i].conn.abort();
            if (slots[i].pcb != nullptr) {
                tcp_arg(slots[i].pcb, nullptr);
                tcp_abort(slots[i].pcb);
            }
            slot_free(&slots[i]);
        }
    }
    if (listener != nullptr) {
        tcp_close(listener);
        listener = nullptr;
    }
    if (st == UP) names_stop();
    // connections closed earlier wait in TIME_WAIT: they go now, and the TCP timer is let run
    // until it stops itself (it is not started again otherwise, when lwIP starts afresh)
    while (tcp_active_pcbs != nullptr) tcp_abort(tcp_active_pcbs);
    while (tcp_tw_pcbs != nullptr) tcp_abort(tcp_tw_pcbs);
    uint32_t t0 = to_ms_since_boot(get_absolute_time());
    for (;;) {
        bool other = false;
        for (struct sys_timeo* t = *sys_timeouts_get_next_timeout(); t != nullptr; t = t->next) {
            if (t->h != lwip_cyclic_timer) other = true;
        }
        if (!other || to_ms_since_boot(get_absolute_time()) - t0 > 1500) break;
        cyw43_arch_poll();
        sleep_ms(5);
    }
    cyw43_arch_deinit();
    started = false;
    st = OFF;
}

bool lend_lwip(uint8_t* arena, size_t size) {
    if (size < MEM_SIZE + 64) return false;
    picomenu_lwip_heap = arena;
    lwip_fresh();                       // (as for the WiFi: the Bluetooth starts it with the chip too)
    return true;
}

State state() { return st; }
const char* address() { return addr; }
const char* name() { return "picomenu"; }
const char* why() { return reason; }

int busy() {
    int n = 0;
    if (slots != nullptr && started) {
        for (int i = 0; i < CONNS; i++) n += slots[i].used ? 1 : 0;
    }
    return n;
}

}   // namespace wifi
