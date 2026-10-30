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

// The Pico W's Bluetooth: an A2DP source (bt.h), with BTstack (btstack_config.h). Only in the
// Pico W build.
//
// The chip is started with cyw43_arch_init, as for the WiFi (which starts BTstack too in this
// build); the two are used one at a time (PicoMenu stops one before starting the other). The
// sound goes as SBC, the codec every A2DP device has: mono at 44.1 kHz when the speaker takes
// it (they all should; else joint stereo, the same sound on both sides), coded here on the Pico
// as it is needed. Every 10 ms a timer works out how many samples are due by the clock, codes
// them and sends them, so the sound keeps to time however the main loop is held up (the
// speaker's own buffer covers a short wait, and what is late is made up at once).
//
// The speaker paired last is remembered (BTstack's store in the flash, which also keeps the
// link key), and connected to again with connect_saved().

#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"
#include "btstack_tlv.h"
#include "hardware/watchdog.h"
#include "bt.h"
#include "wifi.h"

// The variables only needed while the Bluetooth is on go in the part of snap_load_buffer kept
// for it (the Pico W build's memmap_picomenu_w.ld; cleared by start()); the few that say
// whether it is on stay in the ordinary RAM.
#ifdef PICOMENU_BT_SHARED
#define BT_SHARED(name) __attribute__((section(".bss.btshared." #name)))
#else
#define BT_SHARED(name)
#endif

#ifdef PICOMENU_BT_SHARED
extern "C" {
extern char __btshared_start[], __btshared_end[];   // memmap_picomenu_w.ld
}
#endif

namespace bt {

// ---- the crash recorder: in RAM that a restart leaves as it was (not cleared at the start)
namespace {
const uint32_t TRACE_RUN = 0x52554E21, TRACE_FAULT = 0x46415554;
const int TRACE_N = 16;
struct Trace {
    uint32_t magic;                         // TRACE_RUN while running, TRACE_FAULT after a fault
    uint32_t pc, lr, core;
    uint32_t at;                            // events noted (the last TRACE_N are kept)
    uint8_t ev[TRACE_N];
    uint32_t check;
};
Trace __uninitialized_ram(trace);
struct Crash {
    bool hang;
    uint32_t pc, lr, core;
    uint8_t ev[8];
    uint8_t n;
};
Crash last_crash;
bool have_crash = false;

uint32_t trace_sum(const Trace& t) {
    uint32_t x = t.magic ^ t.pc ^ (t.lr << 1) ^ t.core ^ (t.at << 3);
    for (int i = 0; i < TRACE_N; i++) x = (x << 5) ^ (x >> 27) ^ t.ev[i];
    return x;
}
}   // namespace

void note(uint8_t code) {
    if (trace.magic != TRACE_RUN) return;
    trace.ev[trace.at % TRACE_N] = code;
    trace.at++;
    trace.check = trace_sum(trace);
}

// At the start: a fault recorded before the restart (isr_hardfault), or a hang (the watchdog
// restarted it) with the events before it, is kept for crash_text(); the trace starts again
void crash_boot() {
    bool fault = trace.magic == TRACE_FAULT && trace.check == trace_sum(trace);
    bool hang = !fault && watchdog_enable_caused_reboot() && trace.magic == TRACE_RUN && trace.check == trace_sum(trace);
    if (fault || hang) {
        last_crash.hang = hang;
        last_crash.pc = trace.pc;
        last_crash.lr = trace.lr;
        last_crash.core = trace.core;
        uint32_t n = trace.at < 8 ? trace.at : 8;
        last_crash.n = (uint8_t) n;
        for (uint32_t i = 0; i < n; i++) last_crash.ev[i] = trace.ev[(trace.at - n + i) % TRACE_N];
        have_crash = true;
    }
    memset(&trace, 0, sizeof(trace));
    trace.magic = TRACE_RUN;
    trace.check = trace_sum(trace);
}

bool crashed() { return have_crash; }
void crash_clear() { have_crash = false; }

void crash_text(int line, char* out, size_t size) {
    if (!have_crash) {
        out[0] = '\0';
    } else if (line == 0) {
        if (last_crash.hang) snprintf(out, size, "Last restart: a hang");
        else snprintf(out, size, "Crash %lu PC %08lX LR %08lX", (unsigned long) last_crash.core,
            (unsigned long) last_crash.pc, (unsigned long) last_crash.lr);
    } else {
        int k = snprintf(out, size, "Last:");
        for (int i = 0; i < last_crash.n && k + 3 < (int) size; i++) k += snprintf(out + k, size - (size_t) k, " %02X", last_crash.ev[i]);
    }
}

}   // namespace bt

// A fault (either core): where it was is kept (bt::crash_boot shows it after the restart), and
// the Pico restarts, rather than stopping there with the Spectrum's menu waiting on it
extern "C" void __no_inline_not_in_flash_func(picomenu_fault)(uint32_t* frame) {
    using namespace bt;
    trace.magic = TRACE_FAULT;
    trace.pc = frame[6];
    trace.lr = frame[5];
    trace.core = *(volatile uint32_t*) 0xD0000000;     // SIO CPUID: 0 or 1
    trace.check = trace_sum(trace);
    watchdog_reboot(0, 0, 0);
    while (true) {}
}
extern "C" __attribute__((naked)) void isr_hardfault(void) {
    __asm volatile(
        "movs r0, #4\n"
        "mov r1, lr\n"
        "tst r0, r1\n"
        "beq 1f\n"
        "mrs r0, psp\n"
        "b 2f\n"
        "1: mrs r0, msp\n"
        "2: ldr r1, =picomenu_fault\n"
        "bx r1\n");
}

namespace bt {

namespace {

const uint32_t TAG_SAVED = 0x504D5350;      // 'PMSP': the speaker paired last (address, name)
const int AUDIO_MS = 10;
const int SBC_STORAGE = 1030;               // one media packet of SBC frames
const uint8_t INQUIRY_LEN = 8;              // x 1.28 s

State st = OFF;
bool lost_ = false;                         // a connection lost, or not made: start afresh
bool chip = false;                          // cyw43_arch_init done
const char* reason = "";
Source source_ = nullptr;
void* source_ctx = nullptr;

Found list[FOUND_MAX] BT_SHARED(list);
int list_n = 0;
struct NameAsk { uint8_t psrm; uint16_t clock; bool want; };
NameAsk asks[FOUND_MAX] BT_SHARED(asks);
int asking = -1;                            // a name being asked for (remote name request)

struct Saved { uint8_t addr[6]; char name[32]; };
Saved saved BT_SHARED(saved);
bool have_saved = false;
uint8_t peer[6] BT_SHARED(peer);
char peer_name[32] BT_SHARED(peer_name);

btstack_packet_callback_registration_t hci_reg BT_SHARED(hci_reg);
uint8_t sdp_record[150] BT_SHARED(sdp_record);
uint8_t sbc_caps[] = {
    (uint8_t) (((AVDTP_SBC_44100 | AVDTP_SBC_48000) << 4) | AVDTP_SBC_MONO | AVDTP_SBC_JOINT_STEREO | AVDTP_SBC_STEREO),
    0xFF,   // block lengths, subbands, allocation: any
    2, 53   // bitpool
};
uint8_t sbc_config[4] BT_SHARED(sbc_config);
avdtp_stream_endpoint_t* endpoint = nullptr;
btstack_sbc_encoder_state_t encoder BT_SHARED(encoder);

struct Media {
    uint16_t cid;
    uint8_t local_seid;
    bool opened, streaming;
    uint32_t rate;
    bool mono;
    uint32_t last_ms;
    uint32_t frac;                          // samples' thousandths not yet due
    uint32_t ready;                         // samples due, not yet coded
    uint32_t rtp;
    int max_payload;
    btstack_timer_source_t timer;
    uint8_t storage[SBC_STORAGE];
    uint16_t stored;
    bool waiting;                           // a packet asked to be sent
    uint32_t late;                          // the longest gap between timer calls, ms
};
Media m BT_SHARED(m);

void saved_load() {
    const btstack_tlv_t* tlv;
    void* ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    have_saved = tlv != nullptr && tlv->get_tag(ctx, TAG_SAVED, reinterpret_cast<uint8_t*>(&saved), sizeof(saved)) == (int) sizeof(saved);
    if (have_saved) saved.name[sizeof(saved.name) - 1] = '\0';
}

void saved_store() {
    if (have_saved && memcmp(saved.addr, peer, 6) == 0 && strncmp(saved.name, peer_name, sizeof(saved.name)) == 0) {
        return;                             // (the same: the flash is not written each time it connects)
    }
    const btstack_tlv_t* tlv;
    void* ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    memcpy(saved.addr, peer, 6);
    snprintf(saved.name, sizeof(saved.name), "%s", peer_name);
    if (tlv != nullptr) tlv->store_tag(ctx, TAG_SAVED, reinterpret_cast<const uint8_t*>(&saved), sizeof(saved));
    have_saved = true;
}

// ---- the sound

void send_packet() {
    int frame = btstack_sbc_encoder_sbc_buffer_length();
    if (frame <= 0 || m.stored == 0) return;
    m.storage[0] = (uint8_t) (m.stored / frame);        // frames in the packet
    a2dp_source_stream_send_media_payload_rtp(m.cid, m.local_seid, 0, m.rtp, m.storage, m.stored + 1);
    m.rtp += (uint32_t) (m.stored / frame) * (uint32_t) btstack_sbc_encoder_num_audio_frames();
    m.stored = 0;
    m.waiting = false;
}

// The samples due coded into SBC frames, as many as one packet takes
void fill() {
    const uint32_t per = (uint32_t) btstack_sbc_encoder_num_audio_frames();     // 128
    const int frame = btstack_sbc_encoder_sbc_buffer_length();
    while (m.ready >= per && m.max_payload - (int) m.stored >= frame) {
        int16_t pcm[2 * 128];
        int16_t* mono = m.mono ? pcm : pcm + 128;
        if (source_ != nullptr) source_(mono, per, source_ctx);
        else memset(mono, 0, per * 2);
        if (!m.mono) {
            for (uint32_t i = 0; i < per; i++) pcm[2 * i] = pcm[2 * i + 1] = mono[i];
        }
        btstack_sbc_encoder_process_data(pcm);
        memcpy(m.storage + 1 + m.stored, btstack_sbc_encoder_sbc_buffer(), (size_t) frame);
        m.stored = (uint16_t) (m.stored + frame);
        m.ready -= per;
    }
}

void on_timer(btstack_timer_source_t* t) {
    btstack_run_loop_set_timer(t, AUDIO_MS);
    btstack_run_loop_add_timer(t);
    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t gap = m.last_ms != 0 ? now - m.last_ms : AUDIO_MS;
    m.last_ms = now;
    if (gap > m.late) m.late = gap;
    if (gap > 500) gap = 500;               // a long wait (the flash being written): not all made up
    uint32_t due = gap * m.rate + m.frac;
    m.ready += due / 1000;
    m.frac = due % 1000;
    if (m.waiting) return;
    fill();
    if (m.stored > 0 && (int) m.stored + btstack_sbc_encoder_sbc_buffer_length() > m.max_payload) {
        m.waiting = true;
        a2dp_source_stream_endpoint_request_can_send_now(m.cid, m.local_seid);
    }
}

void timer_start() {
    m.max_payload = btstack_min(a2dp_max_media_payload_size(m.cid, m.local_seid), SBC_STORAGE - 1);
    m.stored = 0;
    m.waiting = false;
    m.ready = 0;
    m.frac = 0;
    m.last_ms = 0;
    m.streaming = true;
    btstack_run_loop_remove_timer(&m.timer);
    btstack_run_loop_set_timer_handler(&m.timer, on_timer);
    btstack_run_loop_set_timer(&m.timer, AUDIO_MS);
    btstack_run_loop_add_timer(&m.timer);
}

void timer_stop() {
    m.streaming = false;
    m.stored = 0;
    m.waiting = false;
    btstack_run_loop_remove_timer(&m.timer);
}

// ---- searching

void ask_next_name() {
    if (asking >= 0 || st != SEARCHING) return;
    for (int i = 0; i < list_n; i++) {
        if (asks[i].want) {
            asks[i].want = false;
            asking = i;
            gap_remote_name_request(list[i].addr, asks[i].psrm, (uint16_t) (asks[i].clock | 0x8000));
            return;
        }
    }
    st = READY;                             // all found, all named
}

void on_inquiry_result(uint8_t* packet) {
    uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
    // a speaker, headphones, a receiver ... (major class 4, audio and video), or anything that
    // says it renders audio (the service class)
    if (((cod >> 8) & 0x1F) != 0x04 && (cod & 0x240000) == 0) return;
    bd_addr_t addr;
    gap_event_inquiry_result_get_bd_addr(packet, addr);
    int at = -1;
    for (int i = 0; i < list_n; i++) {
        if (memcmp(list[i].addr, addr, 6) == 0) at = i;
    }
    if (at < 0) {
        if (list_n >= FOUND_MAX) return;
        at = list_n++;
        memcpy(list[at].addr, addr, 6);
        list[at].name[0] = '\0';
        asks[at].want = true;
    }
    asks[at].psrm = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
    asks[at].clock = gap_event_inquiry_result_get_clock_offset(packet);
    if (gap_event_inquiry_result_get_name_available(packet)) {
        int n = gap_event_inquiry_result_get_name_len(packet);
        if (n > (int) sizeof(list[at].name) - 1) n = sizeof(list[at].name) - 1;
        memcpy(list[at].name, gap_event_inquiry_result_get_name(packet), (size_t) n);
        list[at].name[n] = '\0';
        asks[at].want = false;
    }
}

// ---- events

void on_hci(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    bd_addr_t addr;
    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_CONNECTION_COMPLETE:
        note(hci_event_connection_complete_get_status(packet) == 0 ? 0x35 : 0x36);
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        note(0x37);
        break;
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING && st == STARTING) {
            note(0x30);
            st = READY;
            saved_load();
        }
        break;
    case HCI_EVENT_PIN_CODE_REQUEST:
        note(0x33);
        // an older receiver that asks for a PIN: they nearly all use 0000
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        gap_pin_code_response(addr, "0000");
        break;
    case GAP_EVENT_INQUIRY_RESULT:
        if (st == SEARCHING) on_inquiry_result(packet);
        break;
    case GAP_EVENT_INQUIRY_COMPLETE:
        if (st == SEARCHING) ask_next_name();
        break;
    case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
        if (asking >= 0) {
            if (hci_event_remote_name_request_complete_get_status(packet) == 0) {
                snprintf(list[asking].name, sizeof(list[asking].name), "%s",
                    hci_event_remote_name_request_complete_get_remote_name(packet));
            }
            asking = -1;
            ask_next_name();
        }
        break;
    default:
        break;
    }
}

void on_a2dp(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;
    switch (hci_event_a2dp_meta_get_subevent_code(packet)) {
    case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
        if (a2dp_subevent_signaling_connection_established_get_status(packet) != ERROR_CODE_SUCCESS) {
            note(0x21);
            reason = "Could not connect";
            m.cid = 0;
            st = READY;
            lost_ = true;
            break;
        }
        note(0x20);
        m.cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
        break;
    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CAPABILITY: {
        // mono and 44.1 kHz if the speaker takes them (the least to code), before BTstack
        // chooses the settings (a2dp.c, straight after this)
        uint8_t modes = a2dp_subevent_signaling_media_codec_sbc_capability_get_channel_mode_bitmap(packet);
        uint8_t rates = a2dp_subevent_signaling_media_codec_sbc_capability_get_sampling_frequency_bitmap(packet);
        if (endpoint != nullptr) {
            avdtp_set_preferred_channel_mode(endpoint, (modes & AVDTP_SBC_MONO) ? AVDTP_SBC_MONO :
                (modes & AVDTP_SBC_JOINT_STEREO) ? AVDTP_SBC_JOINT_STEREO : AVDTP_SBC_STEREO);
            avdtp_set_preferred_sampling_frequency(endpoint, (rates & AVDTP_SBC_44100) ? 44100 : 48000);
        }
        break;
    }
    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
        note(0x22);
        if (a2dp_subevent_signaling_media_codec_sbc_configuration_get_a2dp_cid(packet) != m.cid) break;
        m.rate = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
        avdtp_channel_mode_t mode = (avdtp_channel_mode_t) a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet);
        btstack_sbc_channel_mode_t cm = SBC_CHANNEL_MODE_JOINT_STEREO;
        switch (mode) {
        case AVDTP_CHANNEL_MODE_MONO: cm = SBC_CHANNEL_MODE_MONO; break;
        case AVDTP_CHANNEL_MODE_STEREO: cm = SBC_CHANNEL_MODE_STEREO; break;
        case AVDTP_CHANNEL_MODE_DUAL_CHANNEL: cm = SBC_CHANNEL_MODE_DUAL_CHANNEL; break;
        default: break;
        }
        m.mono = cm == SBC_CHANNEL_MODE_MONO;
        btstack_sbc_encoder_init(&encoder, SBC_MODE_STANDARD,
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet),
            a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet),
            (btstack_sbc_allocation_method_t) (a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet) - 1),
            m.rate, a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet), cm);
        break;
    }
    case A2DP_SUBEVENT_STREAM_ESTABLISHED:
        if (a2dp_subevent_stream_established_get_status(packet) != ERROR_CODE_SUCCESS) {
            note(0x24);
            reason = "The speaker would not take the sound";
            st = READY;
            lost_ = true;
            break;
        }
        note(0x23);
        m.local_seid = a2dp_subevent_stream_established_get_local_seid(packet);
        m.opened = true;
        reason = "";
        st = CONNECTED;
        saved_store();
        a2dp_source_start_stream(m.cid, m.local_seid);
        break;
    case A2DP_SUBEVENT_STREAM_STARTED:
        note(0x25);
        timer_start();
        break;
    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
        send_packet();
        break;
    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        note(0x26);
        // the speaker paused it (a button on it): started again, as the tape goes on
        timer_stop();
        if (m.opened) a2dp_source_start_stream(m.cid, m.local_seid);
        break;
    case A2DP_SUBEVENT_STREAM_RELEASED:
        note(0x27);
        timer_stop();
        m.opened = false;
        break;
    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
        note(0x28);
        timer_stop();
        m.opened = false;
        m.cid = 0;
        if (st == CONNECTED || st == CONNECTING) {
            if (st == CONNECTING) reason = "Could not connect";
            else reason = "The receiver went away";
            st = READY;
        }
        lost_ = true;
        break;
    default:
        break;
    }
}

void profiles_init() {
    l2cap_init();
    sdp_init();
    a2dp_source_init();
    a2dp_source_register_packet_handler(on_a2dp);
    endpoint = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC, sbc_caps, sizeof(sbc_caps),
        sbc_config, sizeof(sbc_config));
    m = Media();
    if (endpoint != nullptr) m.local_seid = avdtp_local_seid(endpoint);
    memset(sdp_record, 0, sizeof(sdp_record));
    a2dp_source_create_sdp_record(sdp_record, 0x10001, AVDTP_SOURCE_FEATURE_MASK_PLAYER, nullptr, nullptr);
    sdp_register_service(sdp_record);
    gap_set_local_name("PicoMenu");
    gap_set_class_of_device(0x200408);      // audio service, a "player" (as a phone's music player)
    gap_discoverable_control(0);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    hci_reg.callback = on_hci;
    hci_add_event_handler(&hci_reg);
}

}   // namespace

void radio_clear() {
#ifdef PICOMENU_BT_SHARED
    memset(__btshared_start, 0, (size_t) (__btshared_end - __btshared_start));
#endif
}

const char* start(uint8_t* arena, size_t size, Source source, void* ctx) {
    source_ = source;
    source_ctx = ctx;
    if (chip) return nullptr;
    note(0x01);
    reason = "";
    lost_ = false;
    if (!wifi::lend_lwip(arena, size)) {
        st = FAILED;
        reason = "Not enough memory";
        return reason;
    }
    radio_clear();
    list_n = 0;
    asking = -1;
    have_saved = false;
    endpoint = nullptr;
    if (cyw43_arch_init() != 0) {
        st = FAILED;
        reason = "The radio chip did not start";
        return reason;
    }
    chip = true;
    profiles_init();
    st = STARTING;
    hci_power_control(HCI_POWER_ON);
    return nullptr;
}

void poll() {
    if (chip) cyw43_arch_poll();
}

void stop() {
    note(0x02);
    if (!chip) {
        st = OFF;
        return;
    }
    timer_stop();
    if (m.cid != 0) {
        a2dp_source_disconnect(m.cid);
        uint32_t t0 = to_ms_since_boot(get_absolute_time());
        while (m.cid != 0 && to_ms_since_boot(get_absolute_time()) - t0 < 1500) {
            cyw43_arch_poll();
            sleep_ms(5);
        }
    }
    hci_remove_event_handler(&hci_reg);
    cyw43_arch_deinit();
    chip = false;
    st = OFF;
    endpoint = nullptr;
}

State state() { return st; }
const char* why() { return reason; }

void search() {
    if (st != READY) return;
    list_n = 0;
    asking = -1;
    reason = "";
    note(0x03);
    st = SEARCHING;
    if (gap_inquiry_start(INQUIRY_LEN) != 0) st = READY;
}

int found_count() { return list_n; }
const Found& found(int i) { return list[i]; }

static void connect_to(const uint8_t* addr, const char* name) {
    if (st == SEARCHING) {
        gap_inquiry_stop();
        st = READY;
    }
    if (st != READY) return;
    memcpy(peer, addr, 6);
    snprintf(peer_name, sizeof(peer_name), "%s", name[0] != '\0' ? name : bd_addr_to_str(peer));
    reason = "";
    st = CONNECTING;
    note(0x04);
    uint16_t cid = 0;
    bd_addr_t a;
    memcpy(a, peer, 6);
    if (a2dp_source_establish_stream(a, &cid) != ERROR_CODE_SUCCESS) {
        note(0x05);
        reason = "Could not connect";
        st = READY;
        lost_ = true;
        return;
    }
    m.cid = cid;
}

void connect(int i) {
    if (i < 0 || i >= list_n) return;
    connect_to(list[i].addr, list[i].name);
}

bool connect_saved() {
    if (!have_saved) return false;
    connect_to(saved.addr, saved.name);
    return true;
}

void disconnect() {
    note(0x06);
    if (m.cid != 0) a2dp_source_disconnect(m.cid);
}

bool lost() { return lost_; }

const char* saved_name() { return have_saved ? saved.name : ""; }

void forget() {
    if (!have_saved) return;
    bd_addr_t a;
    memcpy(a, saved.addr, 6);
    gap_drop_link_key_for_bd_addr(a);
    const btstack_tlv_t* tlv;
    void* ctx;
    btstack_tlv_get_instance(&tlv, &ctx);
    if (tlv != nullptr) tlv->delete_tag(ctx, TAG_SAVED);
    have_saved = false;
}

const char* connected_name() { return peer_name; }
uint32_t rate() { return m.rate; }
bool mono() { return m.mono; }
uint32_t late_ms() { return m.late; }

}   // namespace bt
