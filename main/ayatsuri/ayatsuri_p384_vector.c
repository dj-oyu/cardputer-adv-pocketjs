#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "ayatsuri_p384_profile.h"

#include "esp_log.h"
#include "mbedtls/private/ecdsa.h"

/* RFC 6979 A.2.6, SHA-384 over "sample". Only verification inputs are used. */
bool ay_p384_vector_run(bool detailed)
{
    static const char x[] =
        "EC3A4E415B4E19A4568618029F427FA5DA9A8BC4AE92E02E06AAE5286B300C64"
        "DEF8F0EA9055866064A254515480BC13";
    static const char y[] =
        "8015D9B72D7D57244EA8EF9AC0C621896708A59367F9DFB9F54CA84B3F1C9DB1"
        "288B231C3AE0D4FE7344FD2533264720";
    static const char sig_r[] =
        "94EDBB92A5ECB8AAD4736E56C691916B3F88140666CE9FA73D64C4EA95AD133C"
        "81A648152E44ACF96E36DD1E80FABE46";
    static const char sig_s[] =
        "99EF4AEB15F178CEA1FE40DB2603138F130E740A19624526203B6351D0A3A94F"
        "A329C145786E679E7B82C71A38628AC8";
    unsigned char digest[] = {
        0x9a, 0x90, 0x83, 0x50, 0x5b, 0xc9, 0x22, 0x76, 0xae, 0xc4, 0xbe, 0x31,
        0x26, 0x96, 0xef, 0x7b, 0xf3, 0xbf, 0x60, 0x3f, 0x4b, 0xbd, 0x38, 0x11,
        0x96, 0xa0, 0x29, 0xf3, 0x40, 0x58, 0x53, 0x12, 0x31, 0x3b, 0xca, 0x4a,
        0x9b, 0x5b, 0x89, 0x0e, 0xfe, 0xe4, 0x2c, 0x77, 0xb1, 0xee, 0x25, 0xfe,
    };
    mbedtls_ecp_group group;
    mbedtls_ecp_point key;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&group);
    mbedtls_ecp_point_init(&key);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    bool ok = !mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP384R1) &&
              !mbedtls_ecp_point_read_string(&key, 16, x, y) &&
              !mbedtls_mpi_read_string(&r, 16, sig_r) &&
              !mbedtls_mpi_read_string(&s, 16, sig_s);
    if (ok) {
        ay_p384_scope_begin(detailed);
        const int valid = mbedtls_ecdsa_verify(&group, digest, sizeof(digest), &key, &r, &s);
        ay_p384_scope_end();
        ay_p384_report("rfc6979_valid");
        digest[0] ^= 1;
        ay_p384_scope_begin(false);
        const int invalid = mbedtls_ecdsa_verify(&group, digest, sizeof(digest), &key, &r, &s);
        ay_p384_scope_end();
        ay_p384_report("rfc6979_altered");
        ok = valid == 0 && invalid == MBEDTLS_ERR_ECP_VERIFY_FAILED;
        ESP_LOGI("P384_VECTOR", "valid_rc=%d invalid_rc=%d pass=%u", valid, invalid, (unsigned)ok);
    }
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&key);
    mbedtls_ecp_group_free(&group);
    return ok;
}
