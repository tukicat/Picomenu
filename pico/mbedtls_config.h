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

// mbedtls's settings for PicoMenu's Pico W build (Mbed TLS 2.28, from the Pico SDK): only what
// a TLS 1.2 client needs to fetch from ZXDB's search (api.zxinfo.dk), Spectrum Computing and
// archive.org (net.h): ECDHE key exchange with RSA or ECDSA certificates, AES-GCM, SHA-256 and
// SHA-384 (no SHA-1). Its memory comes from lwIP's heap (lent from snap_load_buffer while the
// WiFi is on: MBEDTLS_PLATFORM_MEMORY makes lwIP's altcp_tls give it mem_calloc), and is kept
// small: a 16 KB record in (a server may send that much at once), 2 KB out, the AES tables in
// the flash. No date, so certificates' dates are not checked.

#ifndef PICOMENU_MBEDTLS_CONFIG_H
#define PICOMENU_MBEDTLS_CONFIG_H

// the platform: the Pico's random numbers (pico_mbedtls: mbedtls_hardware_poll), memory from lwIP
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_HAVE_TIME           // (a running clock for sessions; no date: MBEDTLS_HAVE_TIME_DATE is off)
// the clock: seconds since the Pico started (net.cxx). The C library's time() (FatFs's rtc.c)
// would bring in mktime and the time zones, 40 KB of the flash
#define MBEDTLS_PLATFORM_TIME_MACRO picomenu_mbedtls_time
#include <time.h>
#ifdef __cplusplus
extern "C"
#endif
time_t picomenu_mbedtls_time(time_t* t);

// the building blocks
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM
// (the curve sums blind a secret with the RNG they are given; without this, a check of a
// signature, which has no secret, seeds one of its own, 1.3 KB more of the stack)
#define MBEDTLS_ECP_NO_INTERNAL_RNG
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

// TLS 1.2, as a client
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH
#define MBEDTLS_SSL_CIPHERSUITES \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, \
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384

// small
#define MBEDTLS_SSL_IN_CONTENT_LEN          16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN         2048
#define MBEDTLS_MPI_MAX_SIZE                512     // 4096-bit RSA
#define MBEDTLS_MPI_WINDOW_SIZE             1
#define MBEDTLS_ECP_WINDOW_SIZE             2
#define MBEDTLS_ECP_FIXED_POINT_OPTIM       0
#define MBEDTLS_ECP_MAX_BITS                384
#define MBEDTLS_X509_MAX_INTERMEDIATE_CA    4
#define MBEDTLS_ENTROPY_MAX_SOURCES         2

#include "mbedtls/check_config.h"

#endif
