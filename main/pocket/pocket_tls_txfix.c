// A one-argument bug in ESP-IDF v6.0.1's dynamic-buffer TLS port, corrected at
// link time rather than by editing the IDF checkout.
//
// components/mbedtls/port/dynamic/esp_mbedtls_dynamic_impl.c:66 defines
//
//     static int tx_buffer_len(mbedtls_ssl_context *ssl, int len) {
//         if (!len) return MBEDTLS_SSL_OUT_BUFFER_LEN;
//         else      return len + MBEDTLS_SSL_HEADER_LEN + MBEDTLS_MAX_IV_LENGTH
//                             + MBEDTLS_SSL_MAC_ADD + MBEDTLS_SSL_PADDING_ADD
//                             + MBEDTLS_SSL_MAX_CID_EXPANSION;
//     }
//
// so a zero means "the whole buffer" and anything else is a *content* length
// that the record overhead still has to be added to. Eight call sites in
// esp_ssl_cli.c pass MBEDTLS_SSL_OUT_BUFFER_LEN instead -- a number that
// already contains that overhead -- so it is added twice:
//
//     MBEDTLS_SSL_OUT_BUFFER_LEN   4429   13 header + 320 overhead + 4096 content
//     tx_buffer_len adds again    + 333   13 + 16 IV + 48 MAC + 256 padding
//     SSL_BUF_HEAD_OFFSET_SIZE    +   8
//                                  ----
//                                  4770   which is what the board measured as
//                                         mbedTLS's largest single allocation
//
// 674 bytes of that buy nothing, and this block is taken and released six times
// in one handshake, so it is also six chances to leave a hole. Passing 0 gets
// the same buffer without the second helping.
//
// Why a linker wrap and not a patched IDF: C:\esp\v6.0.1\esp-idf is shared by
// every project on this machine and is replaced wholesale on an IDF update, so
// a local edit there is both wider than this project and silently temporary.
// esp_mbedtls_add_tx_buffer has external linkage (esp_mbedtls_dynamic_impl.h:80)
// and main/CMakeLists.txt already wraps a symbol for the allocation tracing, so
// this costs one more --wrap and lives with the code that needs it.
//
// The rewrite is exact equality, which is safe rather than lucky:
// CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN is 4096 here, so a genuine content length
// can never be 4429. The one site that computes a real length --
// esp_ssl_cli.c:122-136, for a client certificate -- takes MAX(computed,
// MBEDTLS_SSL_OUT_BUFFER_LEN), so a chain larger than the buffer still arrives
// as its own number and still gets the overhead it needs.

#include "mbedtls/ssl.h"
#include "ssl_misc.h"   // MBEDTLS_SSL_OUT_BUFFER_LEN; mbedTLS 4 keeps it in library/
#include <stddef.h>

int __real_esp_mbedtls_add_tx_buffer(mbedtls_ssl_context *ssl, size_t buffer_len);

int __wrap_esp_mbedtls_add_tx_buffer(mbedtls_ssl_context *ssl, size_t buffer_len) {
    if(buffer_len == MBEDTLS_SSL_OUT_BUFFER_LEN) buffer_len = 0;
    return __real_esp_mbedtls_add_tx_buffer(ssl, buffer_len);
}
