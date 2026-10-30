// BTstack's settings for PicoMenu's Pico W build (bt.cxx): Bluetooth Classic, as an A2DP
// source (the tape's sound sent to a Bluetooth speaker or receiver), one connection at a time.
// Everything is in fixed pools (no malloc), sized for that and no more, as RAM is short.
#ifndef PICOMENU_BTSTACK_CONFIG_H
#define PICOMENU_BTSTACK_CONFIG_H

#ifndef ENABLE_CLASSIC
#define ENABLE_CLASSIC
#endif

// the controller's buffers (the CYW43439): an ACL packet's payload, as 3-DH5 allows
#define HCI_OUTGOING_PRE_BUFFER_SIZE        4
#define HCI_ACL_PAYLOAD_SIZE                1021
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT        4

// one speaker: its connection, AVDTP's signalling and media channels, SDP (asking the
// speaker what it is, and its asking us). Room for two of each: an old connection still
// closing (the receiver gone off, out of range, or to a phone) and a new one
#define MAX_NR_HCI_CONNECTIONS              2
#define MAX_NR_L2CAP_SERVICES               3
#define MAX_NR_L2CAP_CHANNELS               8
#define MAX_NR_AVDTP_STREAM_ENDPOINTS       1
#define MAX_NR_AVDTP_CONNECTIONS            2
#define MAX_NR_SERVICE_RECORD_ITEMS         1
#define MAX_NR_RFCOMM_MULTIPLEXERS          0
#define MAX_NR_RFCOMM_SERVICES              0
#define MAX_NR_RFCOMM_CHANNELS              0
#define MAX_NR_AVRCP_CONNECTIONS            0
#define MAX_NR_AVRCP_BROWSING_CONNECTIONS   0
#define MAX_NR_BNEP_SERVICES                0
#define MAX_NR_BNEP_CHANNELS                0
#define MAX_NR_GOEP_SERVER_SERVICES         0
#define MAX_NR_GOEP_SERVER_CONNECTIONS      0
#define MAX_NR_HFP_CONNECTIONS              0
#define MAX_NR_HID_HOST_CONNECTIONS         0
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 0
#define MAX_NR_WHITELIST_ENTRIES            0
#define MAX_NR_SM_LOOKUP_ENTRIES            0

// the pairing with a speaker (its link key) is kept in the flash (btstack_flash_bank.c, in
// the 8 KB the Pico W build keeps before the flash's file system: CMakeLists.txt)
#define NVM_NUM_LINK_KEYS                   4

#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS         1000
#define ENABLE_SOFTWARE_AES128
#define ENABLE_PRINTF_HEXDUMP               // (only so the SDK's HCI dump file builds; it is not used)

#endif
