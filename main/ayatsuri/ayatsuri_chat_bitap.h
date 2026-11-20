#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* The probe owns a writable, length-bounded SSE line. Keep the input intact
 * on a miss so the reference decoder can read the original JSON. */
typedef struct {
    char *at;
    const char *end;
    char *content;
    char *content_end;
    uint32_t sequence;
    bool have_sequence;
    bool escaped_content;
} ay_chat_scan;

typedef enum {
    AY_CHAT_OTHER, AY_CHAT_ROOT, AY_CHAT_CHOICES, AY_CHAT_CHOICE,
    AY_CHAT_DELTA, AY_CHAT_TEXT, AY_CHAT_SEQUENCE
} ay_chat_path;

static inline unsigned ay_chat_byte(const ay_chat_scan *scan)
{
    return scan->at == scan->end ? 0 : (unsigned char)*scan->at;
}

static inline bool ay_chat_space(unsigned byte)
{
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

static inline void ay_chat_skip_space(ay_chat_scan *scan)
{
    while (ay_chat_space(ay_chat_byte(scan))) ++scan->at;
}

static inline bool ay_chat_take(ay_chat_scan *scan, unsigned byte)
{
    if (ay_chat_byte(scan) != byte) return false;
    ++scan->at;
    return true;
}

static inline bool ay_chat_digit(unsigned byte)
{
    return byte >= '0' && byte <= '9';
}

static inline bool ay_chat_hex4(const char **at, const char *end, unsigned *code)
{
    if (end - *at < 4) return false;
    unsigned value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned ch = (unsigned char)*(*at)++;
        if (ay_chat_digit(ch)) ch -= '0';
        else {
            ch |= 32;
            if (ch < 'a' || ch > 'f') return false;
            ch = ch - 'a' + 10;
        }
        value = (value << 4) | ch;
    }
    *code = value;
    return true;
}

static inline bool ay_chat_escape(const char **at, const char *end, unsigned *code)
{
    if (*at == end) return false;
    switch (*(*at)++) {
    case '"': *code = '"'; return true;
    case '\\': *code = '\\'; return true;
    case '/': *code = '/'; return true;
    case 'b': *code = '\b'; return true;
    case 'f': *code = '\f'; return true;
    case 'n': *code = '\n'; return true;
    case 'r': *code = '\r'; return true;
    case 't': *code = '\t'; return true;
    case 'u':
        if (!ay_chat_hex4(at, end, code) || (*code >= 0xdc00 && *code <= 0xdfff))
            return false;
        if (*code >= 0xd800 && *code <= 0xdbff) {
            unsigned low;
            if (end - *at < 2 || (*at)[0] != '\\' || (*at)[1] != 'u') return false;
            *at += 2;
            if (!ay_chat_hex4(at, end, &low) || low < 0xdc00 || low > 0xdfff) return false;
            *code = 0x10000 + ((*code - 0xd800) << 10) + low - 0xdc00;
        }
        return true;
    default: return false;
    }
}

static inline const char *ay_chat_quote(const char *at, const char *end, bool *escaped)
{
    *escaped = false;
    while (at < end) {
        unsigned byte = (unsigned char)*at++;
        if (byte == '"') return at - 1;
        if (byte < 32) return NULL;
        if (byte == '\\') {
            unsigned code;
            *escaped = true;
            /* Embedded NUL would make the cJSON comparator truncate text. */
            if (!ay_chat_escape(&at, end, &code) || !code) return NULL;
        }
    }
    return NULL;
}

static inline char *ay_chat_utf8(char *out, unsigned code)
{
    if (code >= 0x10000) *out++ = (char)(0xf0 | (code >> 18));
    if (code >= 0x800)
        *out++ = (char)((code >= 0x10000 ? 0x80 : 0xe0) | ((code >> 12) & 0x3f));
    if (code >= 0x80)
        *out++ = (char)((code >= 0x800 ? 0x80 : 0xc0) | ((code >> 6) & 0x3f));
    *out++ = (char)(code < 0x80 ? code : 0x80 | (code & 0x3f));
    return out;
}

/* Shift-And for quote/colon/quote, fed only the three structural bytes.
 * Skip JSON whitespace between tokens. Bit 1 validates the member separator,
 * and bit 2 identifies a string-valued field without scanning string data. */
static inline unsigned ay_chat_separator(ay_chat_scan *scan)
{
    static const uint8_t mask[128] = { ['"'] = 5, [':'] = 2 };
    unsigned state = 1; /* Closing key quote already consumed. */
    ay_chat_skip_space(scan);
    unsigned byte = ay_chat_byte(scan);
    state = ((state << 1) | 1) & (byte < sizeof mask ? mask[byte] : 0);
    if (!(state & 2)) return 0;
    ++scan->at;
    ay_chat_skip_space(scan);
    byte = ay_chat_byte(scan);
    return 2 | (((state << 1) | 1) & (byte < sizeof mask ? mask[byte] : 0) & 4);
}

static inline ay_chat_path ay_chat_member(ay_chat_path path, const char *key, size_t size)
{
    if (path == AY_CHAT_ROOT && size == 1 && *key == 'n') return AY_CHAT_SEQUENCE;
    if (path == AY_CHAT_ROOT && size == 7 && !memcmp(key, "choices", 7)) return AY_CHAT_CHOICES;
    if (path == AY_CHAT_CHOICE && size == 5 && !memcmp(key, "delta", 5)) return AY_CHAT_DELTA;
    if (path == AY_CHAT_DELTA && size == 7 && !memcmp(key, "content", 7)) return AY_CHAT_TEXT;
    return AY_CHAT_OTHER;
}

static inline bool ay_chat_number(ay_chat_scan *scan)
{
    (void)ay_chat_take(scan, '-');
    if (!ay_chat_take(scan, '0')) {
        if (!ay_chat_digit(ay_chat_byte(scan))) return false;
        do { ++scan->at; } while (ay_chat_digit(ay_chat_byte(scan)));
    }
    if (ay_chat_take(scan, '.')) {
        if (!ay_chat_digit(ay_chat_byte(scan))) return false;
        do { ++scan->at; } while (ay_chat_digit(ay_chat_byte(scan)));
    }
    if (ay_chat_take(scan, 'e') || ay_chat_take(scan, 'E')) {
        if (!ay_chat_take(scan, '+')) (void)ay_chat_take(scan, '-');
        if (!ay_chat_digit(ay_chat_byte(scan))) return false;
        do { ++scan->at; } while (ay_chat_digit(ay_chat_byte(scan)));
    }
    return true;
}

static bool ay_chat_value(ay_chat_scan *scan, ay_chat_path path, unsigned depth)
{
    if (depth == 12) return false;
    const unsigned byte = ay_chat_byte(scan);
    if (path == AY_CHAT_SEQUENCE) {
        if (scan->have_sequence || !ay_chat_digit(byte)) return false;
        uint32_t sequence = 0;
        do {
            const unsigned digit = (unsigned char)*scan->at++ - '0';
            if (sequence > (UINT32_MAX - digit) / 10u) return false;
            sequence = sequence * 10u + digit;
        } while (byte != '0' && ay_chat_digit(ay_chat_byte(scan)));
        scan->sequence = sequence;
        scan->have_sequence = true;
        return true;
    }
    if (byte == '{' && (path == AY_CHAT_ROOT || path == AY_CHAT_CHOICE ||
                        path == AY_CHAT_DELTA || path == AY_CHAT_OTHER)) {
        ++scan->at;
        unsigned seen = 0;
        ay_chat_skip_space(scan);
        if (ay_chat_take(scan, '}')) return true;
        do {
            ay_chat_skip_space(scan);
            if (!ay_chat_take(scan, '"')) return false;
            const char *key = scan->at;
            bool escaped;
            const char *quote = ay_chat_quote(key, scan->end, &escaped);
            if (!quote || (escaped && path != AY_CHAT_OTHER)) return false;
            const ay_chat_path member = ay_chat_member(path, key, (size_t)(quote - key));
            if (member != AY_CHAT_OTHER) {
                if (seen & (1u << member)) return false;
                seen |= 1u << member;
            }
            scan->at = (char *)quote + 1;
            const unsigned separator = ay_chat_separator(scan);
            if (!separator || (member == AY_CHAT_TEXT && !(separator & 4)) ||
                !ay_chat_value(scan, member, depth + 1)) return false;
            ay_chat_skip_space(scan);
        } while (ay_chat_take(scan, ','));
        return ay_chat_take(scan, '}');
    }
    if (byte == '[' && (path == AY_CHAT_CHOICES || path == AY_CHAT_OTHER)) {
        ++scan->at;
        ay_chat_skip_space(scan);
        if (ay_chat_take(scan, ']')) return true;
        ay_chat_path item = path == AY_CHAT_CHOICES ? AY_CHAT_CHOICE : AY_CHAT_OTHER;
        do {
            ay_chat_skip_space(scan);
            if (!ay_chat_value(scan, item, depth + 1)) return false;
            item = AY_CHAT_OTHER;
            ay_chat_skip_space(scan);
        } while (ay_chat_take(scan, ','));
        return ay_chat_take(scan, ']');
    }
    if (byte == '"' && (path == AY_CHAT_TEXT || path == AY_CHAT_OTHER)) {
        char *value = ++scan->at;
        bool escaped;
        const char *quote = ay_chat_quote(value, scan->end, &escaped);
        if (!quote) return false;
        scan->at = (char *)quote + 1;
        if (path == AY_CHAT_TEXT) {
            scan->content = value;
            scan->content_end = (char *)quote;
            scan->escaped_content = escaped;
        }
        return true;
    }
    if (path != AY_CHAT_OTHER) return false;
    const char *literal = byte == 't' ? "true" : byte == 'f' ? "false" : byte == 'n' ? "null" : NULL;
    if (!literal) return ay_chat_number(scan);
    const size_t size = byte == 'f' ? 5 : 4;
    if ((size_t)(scan->end - scan->at) < size || memcmp(scan->at, literal, size)) return false;
    scan->at += size;
    return true;
}

static inline bool ay_chat_bitap_delta(char *json, const char *end, unsigned sequence,
                                       const char **text, unsigned *bytes)
{
    ay_chat_scan scan = {.at = json, .end = end};
    ay_chat_skip_space(&scan);
    if (!ay_chat_value(&scan, AY_CHAT_ROOT, 0)) return false;
    ay_chat_skip_space(&scan);
    if (scan.at != end || !scan.have_sequence || scan.sequence != sequence || !scan.content ||
        scan.content == scan.content_end) return false;

    char *out = scan.content_end;
    if (scan.escaped_content) {
        const char *at = memchr(scan.content, '\\', (size_t)(scan.content_end - scan.content));
        out = (char *)at;
        while (at < scan.content_end) {
            if (*at != '\\') *out++ = *at++;
            else {
                unsigned code = 0;
                ++at;
                /* The complete string was validated before any byte was written. */
                if (!ay_chat_escape(&at, scan.content_end, &code)) return false;
                out = ay_chat_utf8(out, code);
            }
        }
    }
    *text = scan.content;
    *bytes = (unsigned)(out - scan.content);
    return true;
}
