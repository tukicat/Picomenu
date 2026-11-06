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

// HTTPS on the Pico W (net.h): lwIP's DNS, then altcp_tls (mbedtls) on port 443, the request,
// the answer through netget::Reader. All of it runs in lwIP's callbacks, from the main loop's
// cyw43_arch_poll (wifi::poll). mbedtls's memory is lwIP's heap (mbedtls_config.h); the roots it
// trusts (netroots.h) are read where they are in the flash (parse_der_nocopy), about 600 bytes
// each while the TLS settings are made. The certificate is checked "optional" (the answer is
// used either way, and verified() says whether it checked out): the Pico has no clock, and a
// root it lacks should not stop a download of a Spectrum game.

#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "lwip/dns.h"
#include "lwip/mem.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "lwip/tcp.h"
// (altcp_tls's own state: what it holds that mbedtls has not read yet; lwip/src/apps/altcp_tls)
#include "../apps/altcp_tls/altcp_tls_mbedtls_structs.h"
#include "net.h"
#include "netget.h"
#include "netroots.h"

// (in the end of snap_load_buffer kept for the radios' variables, as the Bluetooth's are: the
// two are never on together; cleared each time a radio starts: memmap_picomenu_w.ld)
#define NET_SHARED(name) __attribute__((section(".bss.btshared.net_" #name)))

// a whole TLS record (its header, explicit IV and tag with it) has to fit in the TCP window:
// altcp_tls only opens the window again once mbedtls has read a whole record (lwipopts.h)
static_assert(TCP_WND >= MBEDTLS_SSL_IN_CONTENT_LEN + 5 + 8 + 16, "TCP_WND must hold a whole TLS record");

namespace net {

namespace {

const int REDIRECTS = 4;
const uint32_t TIMEOUT_MS = 20000;          // no answer for this long: given up
const uint32_t TLS_TIMEOUT_MS = 60000;      // the handshake (the Pico's sums for it take seconds)

// the curves offered for the key exchange, quickest first (mbedtls's own order starts with the
// slowest, so a server could pick P-384, about three times P-256's work for the Pico)
const mbedtls_ecp_group_id CURVES[] = { MBEDTLS_ECP_DP_CURVE25519, MBEDTLS_ECP_DP_SECP256R1,
    MBEDTLS_ECP_DP_SECP384R1, MBEDTLS_ECP_DP_NONE };

struct Job {
    netget::Url url;
    netget::Reader reader;
    char request[640];
    char next[netget::URL_MAX];
    ip_addr_t ip;
    uint32_t last_ms;
    uint32_t began_ms, stage_ms;            // the request's start; when this stage began
    uint32_t took[STAGES];                  // how long each stage took (ms), for why()
    uint32_t snd0, ack0, rcv0;              // the TCP connection's counts when the request was sent
    uint8_t redirects;
    uint8_t stage;
    uint8_t tls_step;                       // mbedtls's handshake state when it last changed
    bool due;                               // asked for: poll() starts it
    bool verified;
    bool checked;
};
Job job NET_SHARED(job);
mbedtls_x509_crt roots NET_SHARED(roots);
// The connection: in the ordinary RAM, not with the job. The end of snap_load_buffer, where the
// job is, is used for other things while the radio is off (a 128K snapshot, the Bluetooth), and
// stop() (which web_off calls, on each opening of the menu too) must not take what is left there
// for a connection: 2.5.0 test 17 crashed in altcp_arg so, before a ZXDB lookup.
struct altcp_pcb* job_pcb = nullptr;

State st = IDLE;
const char* reason = "";
char detail_[64];
char io_[64];                               // the connection's bytes after the request (io())
Body body_ = nullptr;
void* ctx_ = nullptr;
struct altcp_tls_config* tls = nullptr;
bool roots_parsed = false;

uint32_t now_ms() { return to_ms_since_boot(get_absolute_time()); }

// The stage the request has got to (the one before it has its time kept)
void stage_to(uint8_t s) {
    uint32_t now = now_ms();
    if (job.stage < STAGES) job.took[job.stage] += now - job.stage_ms;
    job.stage = s;
    job.stage_ms = now;
}

// "DNS0.1 TCP0.2 TLS19.8 s5": how long each stage took, to say where it stopped
void make_detail() {
    static const char* const NAME[STAGES] = { "DNS", "TCP", "TLS", "Wait", "Get" };
    int n = 0;
    detail_[0] = '\0';
    for (int i = 0; i < STAGES && i <= job.stage; i++) {
        uint32_t t = job.took[i] + (i == job.stage ? now_ms() - job.stage_ms : 0);
        n += snprintf(detail_ + n, sizeof(detail_) - (size_t) n, "%s%s%u.%u", n > 0 ? " " : "", NAME[i],
            (unsigned) (t / 1000), (unsigned) (t % 1000 / 100));
        if (n >= (int) sizeof(detail_)) break;
    }
    if (job.stage == TLS && n < (int) sizeof(detail_)) snprintf(detail_ + n, sizeof(detail_) - (size_t) n, " s%u", job.tls_step);
}

void fail(const char* why) {
    if (st == BUSY) make_detail();
    reason = why;
    st = FAILED;
}

// The TCP connection under a request (altcp_tls's inner one), or null
struct tcp_pcb* tcp_of(struct altcp_pcb* p) {
    if (p == nullptr) return nullptr;
    if (job.url.tls) p = p->inner_conn;
    return p != nullptr ? static_cast<struct tcp_pcb*>(p->state) : nullptr;
}

// What the connection did after the request, for a request that failed after it was sent:
// "tx230/230 rx0 w8760 tls0 rd0 m30": bytes sent / acknowledged, received, the receive window,
// bytes TLS holds not read, what mbedtls says when asked to read them, the biggest block (KB)
// lwIP's heap could give
void note_io() {
    struct tcp_pcb* t = tcp_of(job_pcb);
    if (t == nullptr || job.stage < WAIT) return;
    int held = 0, rd = 0;
    if (job.url.tls) {
        altcp_mbedtls_state_t* ts = static_cast<altcp_mbedtls_state_t*>(job_pcb->state);
        if (ts != nullptr) {
            held = (ts->rx != nullptr ? ts->rx->tot_len : 0) + (int) ts->ssl_context.in_left;
            if (held > 0) {
                unsigned char b[32];
                rd = mbedtls_ssl_read(&ts->ssl_context, b, sizeof(b));     // (it is being let go)
            }
        }
    }
    // the biggest block lwIP's heap could still give (KB)
    uint32_t lo = 0, hi = 64 * 1024;
    while (hi - lo > 256) {
        uint32_t mid = (lo + hi) / 2;
        void* m = mem_malloc((mem_size_t) mid);
        if (m != nullptr) { mem_free(m); lo = mid; } else hi = mid;
    }
    snprintf(io_, sizeof(io_), "tx%u/%u rx%u w%u tls%d rd%d m%u", (unsigned) (t->snd_nxt - job.snd0),
        (unsigned) (t->lastack - job.ack0), (unsigned) (t->rcv_nxt - job.rcv0), (unsigned) t->rcv_wnd, held, rd,
        (unsigned) (lo / 1024));
}

void let_go() {
    if (st == BUSY && job_pcb != nullptr) note_io();
    if (job_pcb != nullptr) {
        struct altcp_pcb* p = job_pcb;
        job_pcb = nullptr;
        altcp_arg(p, nullptr);
        altcp_recv(p, nullptr);
        altcp_err(p, nullptr);
        altcp_poll(p, nullptr, 0);
        if (altcp_close(p) != ERR_OK) altcp_abort(p);
    }
}

bool body_cb(const uint8_t* data, size_t n, void*) {
    return body_ == nullptr || body_(data, n, ctx_);
}

void start_request();

// The answer is complete: done, or the next place of a redirection
void finished() {
    let_go();
    if (job.reader.redirect()) {
        if (++job.redirects > REDIRECTS) { fail("Too many redirections"); return; }
        if (!netget::resolve(job.url, job.reader.location(), job.next, sizeof(job.next)) || !job.url.parse(job.next)) {
            fail("A redirection PicoMenu can't follow");
            return;
        }
        start_request();
        return;
    }
    if (job.reader.status() != 200) {
        static char s[32];
        snprintf(s, sizeof(s), "The server said %d", job.reader.status());
        fail(job.reader.status() == 404 ? "Not found on the server" : s);
        return;
    }
    st = DONE;
}

err_t recv_cb(void*, struct altcp_pcb* pcb, struct pbuf* p, err_t err) {
    if (pcb != job_pcb) {
        if (p != nullptr) pbuf_free(p);
        return ERR_OK;
    }
    job.last_ms = now_ms();
    if (p == nullptr || err != ERR_OK) {
        // the server closed the connection: the end, if the answer had no length
        if (p != nullptr) pbuf_free(p);
        if (job.reader.closed() || job.reader.redirect()) finished();
        else { let_go(); fail(job.reader.why()); }
        return ERR_OK;
    }
    if (job.stage < GET) stage_to(GET);
    if (!job.checked) {
        // the handshake is over: was the certificate one of the roots'?
        mbedtls_ssl_context* ssl = static_cast<mbedtls_ssl_context*>(altcp_tls_context(pcb));
        job.verified = ssl != nullptr && mbedtls_ssl_get_verify_result(ssl) == 0;
        job.checked = true;
    }
    bool ok = true;
    for (struct pbuf* q = p; q != nullptr && ok; q = q->next) {
        ok = job.reader.feed(static_cast<const uint8_t*>(q->payload), q->len);
    }
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (!ok) {
        let_go();
        fail(job.reader.why()[0] != '\0' ? job.reader.why() : "The answer was not understood");
    } else if (job.reader.done()) {
        finished();
    }
    return ERR_OK;
}

void err_cb(void*, err_t err) {
    job_pcb = nullptr;                      // (lwIP has let it go)
    if (st != BUSY) return;
    fail(err == ERR_ABRT ? "The connection was stopped" : err == ERR_RST ? "The server closed the connection" :
        "The connection failed");
}

err_t poll_cb(void*, struct altcp_pcb*) {
    return ERR_OK;                          // (the time limit is net::poll's)
}

err_t connected_cb(void*, struct altcp_pcb* pcb, err_t err) {
    if (pcb != job_pcb) return ERR_OK;
    if (err != ERR_OK) {
        fail("Could not connect");
        return ERR_OK;
    }
    job.last_ms = now_ms();
    stage_to(WAIT);
    if (struct tcp_pcb* t = tcp_of(pcb)) {
        job.snd0 = t->snd_nxt;
        job.ack0 = t->lastack;
        job.rcv0 = t->rcv_nxt;
    }
    size_t n = netget::request(job.url, job.request, sizeof(job.request));
    if (n == 0 || altcp_write(pcb, job.request, (u16_t) n, TCP_WRITE_FLAG_COPY) != ERR_OK || altcp_output(pcb) != ERR_OK) {
        let_go();
        fail("Could not send the request");
    }
    return ERR_OK;
}

void connect_now() {
    struct altcp_pcb* pcb = job.url.tls ? altcp_tls_new(tls, IPADDR_TYPE_V4) : altcp_tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == nullptr) {
        fail("Not enough memory to connect");
        return;
    }
    if (job.url.tls) {
        // the server's name in the handshake (SNI): one address serves many sites
        mbedtls_ssl_context* ssl = static_cast<mbedtls_ssl_context*>(altcp_tls_context(pcb));
        if (ssl != nullptr) mbedtls_ssl_set_hostname(ssl, job.url.host);
    }
    job_pcb = pcb;
    job.checked = false;
    job.verified = !job.url.tls;
    altcp_arg(pcb, nullptr);
    altcp_recv(pcb, recv_cb);
    altcp_err(pcb, err_cb);
    altcp_poll(pcb, poll_cb, 2);
    job.last_ms = now_ms();
    stage_to(TCP);
    job.tls_step = 0;
    if (altcp_connect(pcb, &job.ip, job.url.port, connected_cb) != ERR_OK) {
        let_go();
        fail("Could not connect");
    }
}

void dns_cb(const char*, const ip_addr_t* ip, void*) {
    if (st != BUSY || job_pcb != nullptr) return;
    if (ip == nullptr) {
        fail("The server's name was not found");
        return;
    }
    job.ip = *ip;
    connect_now();
}

void start_request() {
    job.reader.begin(body_cb, nullptr);
    job.last_ms = now_ms();
    stage_to(DNS);
    err_t e = dns_gethostbyname(job.url.host, &job.ip, dns_cb, nullptr);
    if (e == ERR_OK) connect_now();
    else if (e != ERR_INPROGRESS) fail("The server's name was not found");
}

// The TLS settings, made the first time (and kept until stop()): the roots parsed in place
bool tls_ready() {
    if (tls != nullptr) return true;
    tls = altcp_tls_create_config_client(nullptr, 0);
    if (tls == nullptr) return false;
    mbedtls_x509_crt_init(&roots);
    roots_parsed = true;
    int good = 0;
    for (int i = 0; i < netroots::COUNT; i++) {
        if (mbedtls_x509_crt_parse_der_nocopy(&roots, netroots::ROOTS[i].der, netroots::ROOTS[i].len) == 0) good++;
    }
    // (struct altcp_tls_config starts with its mbedtls_ssl_config: altcp_tls_mbedtls.c)
    mbedtls_ssl_config* conf = reinterpret_cast<mbedtls_ssl_config*>(tls);
    if (good > 0) mbedtls_ssl_conf_ca_chain(conf, &roots, nullptr);
    mbedtls_ssl_conf_curves(conf, CURVES);
    return true;
}

}   // namespace

}   // namespace net

// mbedtls's clock (mbedtls_config.h): seconds since the start
extern "C" time_t picomenu_mbedtls_time(time_t* t) {
    time_t now = (time_t) (to_ms_since_boot(get_absolute_time()) / 1000);
    if (t != nullptr) *t = now;
    return now;
}

namespace net {

const char* get(const char* url, Body body, void* ctx) {
    if (st == BUSY) return "Busy";
    let_go();
    memset(&job, 0, sizeof(job));
    job.began_ms = job.stage_ms = now_ms();
    detail_[0] = '\0';
    io_[0] = '\0';
    if (!job.url.parse(url)) return "Not a URL PicoMenu can get";
    body_ = body;
    ctx_ = ctx;
    reason = "";
    st = BUSY;
    // started by poll(): the TLS settings, the DNS and the sending need a deep stack (the main
    // loop gives poll the WiFi's own: picoface.cxx), not the menu's
    job.due = true;
    return nullptr;
}

void poll() {
    if (st != BUSY) return;
    if (job.due) {
        job.due = false;
        job.last_ms = now_ms();
        if (job.url.tls && !tls_ready()) { fail("Not enough memory for HTTPS"); return; }
        start_request();
        return;
    }
    if (job_pcb != nullptr && job.url.tls && job.stage <= TLS) {
        // the handshake: under way once the TCP connection is made (its hello sent)
        mbedtls_ssl_context* ssl = static_cast<mbedtls_ssl_context*>(altcp_tls_context(job_pcb));
        if (ssl != nullptr && ssl->state > MBEDTLS_SSL_CLIENT_HELLO) {
            if (job.stage == TCP) stage_to(TLS);
            if (ssl->state != job.tls_step) {
                job.tls_step = (uint8_t) ssl->state;
                job.last_ms = now_ms();         // (each step of it is progress)
            }
        }
    }
    uint32_t limit = job.stage == TLS ? TLS_TIMEOUT_MS : TIMEOUT_MS;
    if (now_ms() - job.last_ms > limit) {
        static const char* const WHERE[STAGES] = { "Timed out finding the server", "Timed out connecting",
            "Timed out securing it (TLS)", "Timed out waiting for it", "Timed out receiving it" };
        const char* why = WHERE[job.stage < STAGES ? job.stage : 0];
        let_go();
        fail(why);
    }
}

void stop() {
    let_go();
    if (st == BUSY) fail("Stopped");
    if (tls != nullptr) {
        altcp_tls_free_config(tls);
        tls = nullptr;
        // and the random number generator altcp_tls keeps for its configurations (only their
        // count goes down with the configuration): it is in lwIP's heap, which is
        // snap_load_buffer, used for other things once the WiFi is off. Kept, the next
        // configuration would take it up again from memory since written over (2.5.0 test 17:
        // a crash in mbedtls_ctr_drbg_reseed, at a second ZXDB lookup).
        altcp_tls_free_entropy();
    }
    if (roots_parsed) {
        mbedtls_x509_crt_free(&roots);
        roots_parsed = false;
    }
    st = IDLE;
}

State state() { return st; }
const char* why() { return reason; }
int status() { return job.reader.status(); }
int64_t length() { return job.reader.length(); }
uint64_t received() { return job.reader.received(); }
bool verified() { return job.verified; }
const char* host() { return job.url.host; }
const char* detail() { return detail_; }
const char* io() { return io_; }
Stage stage() { return (Stage) job.stage; }
uint32_t elapsed_ms() { return st == BUSY ? now_ms() - job.began_ms : 0; }
void* alloc(size_t n) { return mem_malloc((mem_size_t) n); }
void release(void* p) { mem_free(p); }

}   // namespace net
