/*
 * MDict v2 container reader (MDX dictionary text and MDD resource files).
 *
 * The layout implemented here is the one MdxBuilder 3.x writes:
 *
 *   header     4-byte big-endian length, UTF-16 root element with attributes,
 *              4-byte little-endian Adler-32 of the element bytes;
 *   sections   five big-endian 64-bit numbers (key blocks, entries, decoded
 *              key-block index size, key-block index size, key-block bytes)
 *              plus their Adler-32;
 *   key index  four-byte block descriptor, Adler-32, zlib stream;
 *   key blocks key record: 64-bit record offset + key text + NUL terminator;
 *   records    block table (compressed/decoded size per block) then blocks.
 *
 * Every block starts with a little-endian descriptor holding the compression
 * method (0 stored, 2 zlib), the encryption method (0 none, 1 the MDict block
 * cipher, 2 Salsa-20) and the encrypted prefix length, followed by a
 * big-endian Adler-32 of the decoded bytes. Encrypted blocks with the header
 * flag "2" derive their block key from that checksum through RIPEMD-128, which
 * is why no passcode is needed.
 */

#include "mdict_internal.h"

#include <quirewise/charset.h>
#include <quirewise/compression.h>
#include <quirewise/crypto.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int mdict_fail(char *error, size_t error_capacity, int status,
                      const char *message)
{
    if (error && error_capacity) snprintf(error, error_capacity, "%s", message);
    return status;
}

static uint32_t mdict_load_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static uint32_t mdict_load_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t mdict_load_be64(const uint8_t *bytes)
{
    return ((uint64_t)mdict_load_be32(bytes) << 32) |
           (uint64_t)mdict_load_be32(bytes + 4);
}

static uint16_t mdict_load_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | (uint16_t)bytes[1]);
}

int qw_mdict_buffer_reserve(QwMdictBuffer *buffer, size_t additional)
{
    size_t needed;
    size_t capacity;
    uint8_t *grown;

    if (!buffer) return QW_MDICT_ERR_INVALID_ARGUMENT;
    if (additional > buffer->limit ||
        buffer->size > buffer->limit - additional)
        return QW_MDICT_ERR_RESOURCE_LIMIT;
    needed = buffer->size + additional;
    if (needed <= buffer->capacity) return QW_MDICT_OK;
    capacity = buffer->capacity ? buffer->capacity : 256u;
    while (capacity < needed) {
        if (capacity > buffer->limit / 2u) {
            capacity = buffer->limit;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed) return QW_MDICT_ERR_RESOURCE_LIMIT;
    grown = (uint8_t *)realloc(buffer->data, capacity);
    if (!grown) return QW_MDICT_ERR_OUT_OF_MEMORY;
    buffer->data = grown;
    buffer->capacity = capacity;
    return QW_MDICT_OK;
}

int qw_mdict_buffer_append(QwMdictBuffer *buffer, const void *data, size_t size)
{
    int status;
    if (!buffer || (!data && size)) return QW_MDICT_ERR_INVALID_ARGUMENT;
    if (!size) return QW_MDICT_OK;
    status = qw_mdict_buffer_reserve(buffer, size);
    if (status != QW_MDICT_OK) return status;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return QW_MDICT_OK;
}

int qw_mdict_buffer_append_text(QwMdictBuffer *buffer, const char *text)
{
    return qw_mdict_buffer_append(buffer, text, text ? strlen(text) : 0u);
}

int qw_mdict_buffer_append_byte(QwMdictBuffer *buffer, uint8_t value)
{
    return qw_mdict_buffer_append(buffer, &value, 1u);
}

void qw_mdict_buffer_clear(QwMdictBuffer *buffer)
{
    if (buffer) buffer->size = 0u;
}

void qw_mdict_buffer_free(QwMdictBuffer *buffer)
{
    if (!buffer) return;
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0u;
    buffer->capacity = 0u;
}

/* ------------------------------------------------------------------ */
/* Header decoding                                                     */
/* ------------------------------------------------------------------ */

static int mdict_append_utf8(QwMdictBuffer *buffer, uint32_t code)
{
    uint8_t encoded[4];
    size_t length;

    if (code < 0x80u) {
        encoded[0] = (uint8_t)code;
        length = 1u;
    } else if (code < 0x800u) {
        encoded[0] = (uint8_t)(0xc0u | (code >> 6));
        encoded[1] = (uint8_t)(0x80u | (code & 0x3fu));
        length = 2u;
    } else if (code < 0x10000u) {
        encoded[0] = (uint8_t)(0xe0u | (code >> 12));
        encoded[1] = (uint8_t)(0x80u | ((code >> 6) & 0x3fu));
        encoded[2] = (uint8_t)(0x80u | (code & 0x3fu));
        length = 3u;
    } else if (code <= 0x10ffffu) {
        encoded[0] = (uint8_t)(0xf0u | (code >> 18));
        encoded[1] = (uint8_t)(0x80u | ((code >> 12) & 0x3fu));
        encoded[2] = (uint8_t)(0x80u | ((code >> 6) & 0x3fu));
        encoded[3] = (uint8_t)(0x80u | (code & 0x3fu));
        length = 4u;
    } else {
        return QW_MDICT_ERR_FORMAT;
    }
    return qw_mdict_buffer_append(buffer, encoded, length);
}

/* Decode UTF-16 text into UTF-8 until the first NUL code unit. */
static int mdict_utf16_to_utf8(const uint8_t *bytes, size_t size, int big_endian,
                               QwMdictBuffer *out)
{
    size_t units = size / 2u;
    size_t index = 0u;

    if ((!bytes && size) || !out) return QW_MDICT_ERR_INVALID_ARGUMENT;
    while (index < units) {
        uint32_t unit = big_endian
                            ? ((uint32_t)bytes[index * 2u] << 8) |
                                  (uint32_t)bytes[index * 2u + 1u]
                            : (uint32_t)bytes[index * 2u] |
                                  ((uint32_t)bytes[index * 2u + 1u] << 8);
        uint32_t code = unit;
        int status;

        if (unit == 0u) break;
        if (unit >= 0xd800u && unit <= 0xdbffu && index + 1u < units) {
            uint32_t low = big_endian
                               ? ((uint32_t)bytes[(index + 1u) * 2u] << 8) |
                                     (uint32_t)bytes[(index + 1u) * 2u + 1u]
                               : (uint32_t)bytes[(index + 1u) * 2u] |
                                     ((uint32_t)bytes[(index + 1u) * 2u + 1u]
                                      << 8);
            if (low >= 0xdc00u && low <= 0xdfffu) {
                code = 0x10000u + ((unit - 0xd800u) << 10) + (low - 0xdc00u);
                ++index;
            } else {
                code = 0xfffdu;
            }
        } else if (unit >= 0xdc00u && unit <= 0xdfffu) {
            code = 0xfffdu;
        }
        status = mdict_append_utf8(out, code);
        if (status != QW_MDICT_OK) return status;
        ++index;
    }
    return QW_MDICT_OK;
}

/* Unescape the XML entities MdxBuilder uses inside attribute values. */
int qw_mdict_append_unescaped(QwMdictBuffer *out, const uint8_t *text, size_t size)
{
    size_t index = 0u;

    while (index < size) {
        uint8_t value = text[index];
        if (value == '&') {
            size_t end = index + 1u;
            int status = QW_MDICT_OK;
            while (end < size && end - index <= 10u && text[end] != ';') ++end;
            if (end < size && text[end] == ';') {
                size_t length = end - index - 1u;
                const char *name = (const char *)(text + index + 1u);
                if (length == 3u && memcmp(name, "amp", 3u) == 0)
                    status = qw_mdict_buffer_append_byte(out, '&');
                else if (length == 2u && memcmp(name, "lt", 2u) == 0)
                    status = qw_mdict_buffer_append_byte(out, '<');
                else if (length == 2u && memcmp(name, "gt", 2u) == 0)
                    status = qw_mdict_buffer_append_byte(out, '>');
                else if (length == 4u && memcmp(name, "quot", 4u) == 0)
                    status = qw_mdict_buffer_append_byte(out, '"');
                else if (length == 4u && memcmp(name, "apos", 4u) == 0)
                    status = qw_mdict_buffer_append_byte(out, '\'');
                else if (length == 4u && memcmp(name, "nbsp", 4u) == 0)
                    status = qw_mdict_buffer_append_byte(out, ' ');
                else
                    status = qw_mdict_buffer_append(out, text + index,
                                                    end - index + 1u);
                if (status != QW_MDICT_OK) return status;
                index = end + 1u;
                continue;
            }
        }
        if (qw_mdict_buffer_append_byte(out, value) != QW_MDICT_OK)
            return QW_MDICT_ERR_OUT_OF_MEMORY;
        ++index;
    }
    return QW_MDICT_OK;
}

/* Locate `Name="value"` in the root element; the value may span lines. */
static int mdict_find_attribute(const char *text, size_t size, const char *name,
                                size_t *out_start, size_t *out_size)
{
    size_t name_size = strlen(name);
    size_t index = 0u;

    if (!text || !name || !out_start || !out_size) return 0;
    while (index + name_size + 2u <= size) {
        int boundary = index == 0u;
        if (!boundary) {
            char previous = text[index - 1u];
            boundary = previous == ' ' || previous == '\t' ||
                       previous == '\r' || previous == '\n' ||
                       previous == '<' || previous == '\'';
        }
        if (boundary && memcmp(text + index, name, name_size) == 0 &&
            text[index + name_size] == '=' &&
            text[index + name_size + 1u] == '"') {
            size_t start = index + name_size + 2u;
            size_t end = start;
            while (end < size && text[end] != '"') ++end;
            if (end >= size) return 0;
            *out_start = start;
            *out_size = end - start;
            return 1;
        }
        ++index;
    }
    return 0;
}

/* Copy one optional attribute into an owned UTF-8 string. */
static int mdict_take_attribute(const char *text, size_t size, const char *name,
                                char **out, size_t limit)
{
    QwMdictBuffer value;
    size_t start = 0u;
    size_t length = 0u;

    memset(&value, 0, sizeof(value));
    value.limit = limit;
    *out = NULL;
    if (!mdict_find_attribute(text, size, name, &start, &length)) return 0;
    if (qw_mdict_append_unescaped(&value, (const uint8_t *)text + start, length) !=
        QW_MDICT_OK) {
        qw_mdict_buffer_free(&value);
        return -1;
    }
    if (qw_mdict_buffer_append_byte(&value, 0u) != QW_MDICT_OK) {
        qw_mdict_buffer_free(&value);
        return -1;
    }
    *out = (char *)value.data;
    return 1;
}

/* Copy one attribute into a fixed buffer; missing attributes keep the value. */
static int mdict_read_attribute(const char *text, size_t size, const char *name,
                                char *out, size_t capacity)
{
    QwMdictBuffer value;
    size_t start = 0u;
    size_t length = 0u;
    int status = 0;

    memset(&value, 0, sizeof(value));
    value.limit = capacity > 0u ? capacity - 1u : 0u;
    if (!mdict_find_attribute(text, size, name, &start, &length)) return 0;
    if (qw_mdict_append_unescaped(&value, (const uint8_t *)text + start, length) !=
        QW_MDICT_OK) {
        qw_mdict_buffer_free(&value);
        return -1;
    }
    if (out && capacity) {
        size_t copied = value.size < capacity - 1u ? value.size : capacity - 1u;
        if (copied) memcpy(out, value.data, copied);
        out[copied] = '\0';
        status = 1;
    }
    qw_mdict_buffer_free(&value);
    return status;
}

static int mdict_flag_value(const char *text, size_t size, const char *name,
                            int fallback)
{
    char value[16];
    if (mdict_read_attribute(text, size, name, value, sizeof(value)) <= 0)
        return fallback;
    if (strcmp(value, "Yes") == 0 || strcmp(value, "yes") == 0 ||
        strcmp(value, "1") == 0)
        return 1;
    if (strcmp(value, "No") == 0 || strcmp(value, "no") == 0 ||
        strcmp(value, "0") == 0)
        return 0;
    return fallback;
}

static int mdict_encoding_is_utf16(const char *encoding)
{
    char compact[24];
    size_t index = 0u;
    size_t length;

    if (!encoding) return 0;
    for (length = 0u; encoding[length] && index + 1u < sizeof(compact);
         ++length) {
        char value = encoding[length];
        if (value == '-' || value == '_' || value == ' ' || value == '.')
            continue;
        compact[index++] = (value >= 'a' && value <= 'z')
                               ? (char)(value - 'a' + 'A')
                               : value;
    }
    compact[index] = '\0';
    return index == 5u && memcmp(compact, "UTF16", 5u) == 0;
}

static int mdict_encoding_is_utf8(const char *encoding)
{
    char compact[24];
    size_t index = 0u;
    size_t length;

    if (!encoding || !encoding[0]) return 1;
    for (length = 0u; encoding[length] && index + 1u < sizeof(compact);
         ++length) {
        char value = encoding[length];
        if (value == '-' || value == '_' || value == ' ' || value == '.')
            continue;
        compact[index++] = (value >= 'a' && value <= 'z')
                               ? (char)(value - 'a' + 'A')
                               : value;
    }
    compact[index] = '\0';
    return index == 4u && memcmp(compact, "UTF8", 4u) == 0;
}

static int mdict_parse_header(const uint8_t *bytes, size_t size,
                              int resource_file, QwMdictMeta *meta,
                              size_t *out_header_end, char *error,
                              size_t error_capacity)
{
    uint32_t header_bytes;
    uint32_t stored_adler;
    size_t bom = 0u;
    int big_endian = 0;
    QwMdictBuffer text;
    size_t start = 0u;
    size_t length = 0u;
    int status;

    memset(&text, 0, sizeof(text));
    text.limit = QW_MDICT_MAX_HEADER_BYTES * 2u;
    if (size < 8u)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "file is too short for an MDict header");
    header_bytes = mdict_load_be32(bytes);
    if (header_bytes < 8u || (size_t)header_bytes > QW_MDICT_MAX_HEADER_BYTES)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_RESOURCE_LIMIT,
                          "MDict header size is out of range");
    if ((size_t)header_bytes + 8u > size)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict header runs past the end of the file");

    if (header_bytes >= 2u && bytes[4] == 0xffu && bytes[5] == 0xfeu) {
        bom = 2u;
    } else if (header_bytes >= 2u && bytes[4] == 0xfeu && bytes[5] == 0xffu) {
        bom = 2u;
        big_endian = 1;
    }
    status = mdict_utf16_to_utf8(bytes + 4u + bom, (size_t)header_bytes - bom,
                                 big_endian, &text);
    if (status != QW_MDICT_OK) {
        qw_mdict_buffer_free(&text);
        return mdict_fail(error, error_capacity, status,
                          "MDict header text is not valid UTF-16");
    }
    stored_adler = mdict_load_le32(bytes + 4u + header_bytes);
    if (stored_adler != qw_compression_adler32(bytes + 4u, header_bytes)) {
        qw_mdict_buffer_free(&text);
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict header checksum does not match");
    }

    if (mdict_find_attribute((const char *)text.data, text.size,
                             "GeneratedByEngineVersion", &start, &length)) {
        char version[24];
        size_t copied = length < sizeof(version) - 1u ? length : sizeof(version) - 1u;
        memcpy(version, text.data + start, copied);
        version[copied] = '\0';
        meta->engine_version = strtod(version, NULL);
    } else {
        qw_mdict_buffer_free(&text);
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict header has no engine version");
    }
    if (meta->engine_version < 2.0) {
        qw_mdict_buffer_free(&text);
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_UNSUPPORTED,
                          "MDict engine 1.2 files use LZO blocks and are not "
                          "supported; rebuild with MdxBuilder 3.x");
    }
    if (meta->engine_version >= 3.0) {
        qw_mdict_buffer_free(&text);
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_UNSUPPORTED,
                          "MDict engine 3.0 containers are not supported yet");
    }

    meta->encrypted = 0;
    if (mdict_find_attribute((const char *)text.data, text.size, "Encrypted",
                             &start, &length)) {
        char value[16];
        size_t copied = length < sizeof(value) - 1u ? length : sizeof(value) - 1u;
        memcpy(value, text.data + start, copied);
        value[copied] = '\0';
        if (strcmp(value, "Yes") == 0 || strcmp(value, "yes") == 0) {
            meta->encrypted = 1;
        } else if (strcmp(value, "No") != 0 && strcmp(value, "no") != 0 &&
                   strcmp(value, "") != 0) {
            meta->encrypted = atoi(value);
        }
    }

    if (resource_file) {
        snprintf(meta->encoding, sizeof(meta->encoding), "UTF-16");
        meta->format[0] = '\0';
    } else {
        if (mdict_read_attribute((const char *)text.data, text.size, "Encoding",
                                 meta->encoding, sizeof(meta->encoding)) < 0) {
            qw_mdict_buffer_free(&text);
            return mdict_fail(error, error_capacity,
                              QW_MDICT_ERR_OUT_OF_MEMORY,
                              "out of memory while reading the header");
        }
        if (!meta->encoding[0])
            snprintf(meta->encoding, sizeof(meta->encoding), "UTF-8");
        mdict_read_attribute((const char *)text.data, text.size, "Format",
                             meta->format, sizeof(meta->format));
        if (!meta->format[0])
            snprintf(meta->format, sizeof(meta->format), "Html");
    }
    meta->key_case_sensitive =
        mdict_flag_value((const char *)text.data, text.size,
                         "KeyCaseSensitive", 0);
    meta->strip_key =
        mdict_flag_value((const char *)text.data, text.size, "StripKey", 0);

    if (mdict_take_attribute((const char *)text.data, text.size, "Title",
                             &meta->title, QW_MDICT_MAX_META_BYTES) < 0 ||
        mdict_take_attribute((const char *)text.data, text.size, "Description",
                             &meta->description, QW_MDICT_MAX_META_BYTES) < 0 ||
        mdict_take_attribute((const char *)text.data, text.size, "StyleSheet",
                             &meta->style_sheet,
                             QW_MDICT_MAX_META_BYTES) < 0) {
        qw_mdict_buffer_free(&text);
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_OUT_OF_MEMORY,
                          "out of memory while reading the header");
    }
    qw_mdict_buffer_free(&text);
    *out_header_end = 4u + (size_t)header_bytes + 4u;
    return QW_MDICT_OK;
}

/* ------------------------------------------------------------------ */
/* Block decoding                                                      */
/* ------------------------------------------------------------------ */

/* The MDict block cipher: swap nibbles, XOR with the previous ciphertext
 * byte, the byte index and a 16-byte block key. */
static void mdict_fast_decrypt(uint8_t *data, size_t size,
                               const uint8_t key[16])
{
    uint8_t previous = 0x36u;
    size_t index;

    for (index = 0u; index < size; ++index) {
        uint8_t value =
            (uint8_t)((data[index] >> 4) | (data[index] << 4));
        value = (uint8_t)(value ^ previous ^ (uint8_t)index ^
                          key[index % 16u]);
        previous = data[index];
        data[index] = value;
    }
}

static int mdict_inflate(const uint8_t *input, size_t input_size,
                         size_t decoded_size, uint8_t **out_data,
                         char *error, size_t error_capacity)
{
    QwCompressionInflateStatus status;
    size_t written = 0u;
    uint8_t *decoded;

    if (!decoded_size || decoded_size > QW_MDICT_MAX_BLOCK_BYTES)
        return mdict_fail(error, error_capacity,
                          QW_MDICT_ERR_RESOURCE_LIMIT,
                          "MDict block declares an out-of-range decoded size");
    decoded = (uint8_t *)malloc(decoded_size);
    if (!decoded)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_OUT_OF_MEMORY,
                          "out of memory while decoding an MDict block");
    status = qw_compression_inflate_zlib_bounded(input, input_size, decoded,
                                                 decoded_size, &written);
    if (status != QW_COMPRESSION_INFLATE_OK || written != decoded_size) {
        free(decoded);
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_DECODE,
                          "MDict zlib block is corrupt");
    }
    *out_data = decoded;
    return QW_MDICT_OK;
}

/* Decode one section block: descriptor, checksum, optional decryption, then
 * zlib or stored payload, verified against the checksum of the decoded bytes. */
static int mdict_decode_block(const uint8_t *block, size_t block_size,
                              size_t decoded_size, uint8_t **out_data,
                              char *error, size_t error_capacity)
{
    uint32_t descriptor;
    uint32_t adler;
    uint32_t computed;
    uint8_t key[16];
    uint8_t *payload;
    size_t payload_size;
    unsigned compression;
    unsigned encryption;
    size_t encrypted_size;

    if (!block || block_size < 8u)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict block header is truncated");
    descriptor = mdict_load_le32(block);
    compression = descriptor & 0x0fu;
    encryption = (descriptor >> 4) & 0x0fu;
    encrypted_size = (descriptor >> 8) & 0xffu;
    adler = mdict_load_be32(block + 4u);
    payload_size = block_size - 8u;
    if (encrypted_size > payload_size)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict block declares an invalid encrypted prefix");
    payload = (uint8_t *)malloc(payload_size ? payload_size : 1u);
    if (!payload)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_OUT_OF_MEMORY,
                          "out of memory while decoding an MDict block");
    if (payload_size) memcpy(payload, block + 8u, payload_size);

    if (encryption != 0u) {
        if (encryption == 2u) {
            free(payload);
            return mdict_fail(error, error_capacity,
                              QW_MDICT_ERR_UNSUPPORTED,
                              "MDict Salsa-20 encrypted blocks need the "
                              "registration passcode");
        }
        if (encryption != 1u) {
            free(payload);
            return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                              "MDict block uses an unknown encryption method");
        }
        if (qw_crypto_ripemd128(block + 4u, 4u, key) != QW_CRYPTO_OK) {
            free(payload);
            return mdict_fail(error, error_capacity, QW_MDICT_ERR_DECODE,
                              "MDict block key derivation failed");
        }
        mdict_fast_decrypt(payload, encrypted_size, key);
    }

    if (compression == 0u) {
        if (decoded_size && decoded_size != payload_size) {
            free(payload);
            return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                              "MDict stored block size does not match the index");
        }
        *out_data = payload;
        computed = qw_compression_adler32(payload, payload_size);
        if (computed != adler) {
            free(payload);
            return mdict_fail(error, error_capacity, QW_MDICT_ERR_DECODE,
                              "MDict block checksum does not match");
        }
        return QW_MDICT_OK;
    }
    if (compression == 1u) {
        free(payload);
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_UNSUPPORTED,
                          "MDict LZO blocks belong to engine 1.2 and are not "
                          "supported");
    }
    if (compression != 2u) {
        free(payload);
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict block uses an unknown compression method");
    }
    {
        int status = mdict_inflate(payload, payload_size, decoded_size, out_data,
                                   error, error_capacity);
        free(payload);
        if (status != QW_MDICT_OK) return status;
    }
    computed = qw_compression_adler32(*out_data, decoded_size);
    if (computed != adler) {
        free(*out_data);
        *out_data = NULL;
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_DECODE,
                          "MDict block checksum does not match");
    }
    return QW_MDICT_OK;
}

/* The key-block index uses the same descriptor but is always zlib, and its
 * key derives from the checksum field plus the format's own constant. */
static int mdict_decode_key_info(const uint8_t *block, size_t block_size,
                                 size_t decoded_size, int encrypted,
                                 uint8_t **out_data, char *error,
                                 size_t error_capacity)
{
    uint32_t descriptor;
    uint32_t adler;
    uint32_t computed;
    uint8_t *payload;
    size_t payload_size;
    int status;

    if (!block || block_size < 8u)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict key-block index is truncated");
    descriptor = mdict_load_le32(block);
    if ((descriptor & 0x0fu) != 2u)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_UNSUPPORTED,
                          "MDict key-block index must be zlib-compressed");
    adler = mdict_load_be32(block + 4u);
    payload_size = block_size - 8u;
    payload = (uint8_t *)malloc(payload_size ? payload_size : 1u);
    if (!payload)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_OUT_OF_MEMORY,
                          "out of memory while decoding the key-block index");
    if (payload_size) memcpy(payload, block + 8u, payload_size);
    if (encrypted & 2) {
        uint8_t seed[8];
        uint8_t key[16];
        memcpy(seed, block + 4u, 4u);
        seed[4] = 0x95u;
        seed[5] = 0x36u;
        seed[6] = 0x00u;
        seed[7] = 0x00u;
        if (qw_crypto_ripemd128(seed, sizeof(seed), key) != QW_CRYPTO_OK) {
            free(payload);
            return mdict_fail(error, error_capacity, QW_MDICT_ERR_DECODE,
                              "MDict key-block index key derivation failed");
        }
        mdict_fast_decrypt(payload, payload_size, key);
    }
    status = mdict_inflate(payload, payload_size, decoded_size, out_data, error,
                           error_capacity);
    free(payload);
    if (status != QW_MDICT_OK) return status;
    computed = qw_compression_adler32(*out_data, decoded_size);
    if (computed != adler) {
        free(*out_data);
        *out_data = NULL;
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_DECODE,
                          "MDict key-block index checksum does not match");
    }
    return QW_MDICT_OK;
}

/* ------------------------------------------------------------------ */
/* Key text decoding                                                   */
/* ------------------------------------------------------------------ */

/* Map one stored key or record to UTF-8. UTF-16 keys may carry a byte-order
 * mark; legacy code pages go through the charset package. */
static int mdict_append_converted(QwMdictBuffer *out, const QwMdictMeta *meta,
                                  const uint8_t *bytes, size_t size,
                                  char *error, size_t error_capacity)
{
    int big_endian = 0;
    size_t skip = 0u;

    if (!size) return QW_MDICT_OK;
    if (mdict_encoding_is_utf8(meta->encoding))
        return qw_mdict_buffer_append(out, bytes, size);
    if (mdict_encoding_is_utf16(meta->encoding)) {
        if (size >= 2u && bytes[0] == 0xffu && bytes[1] == 0xfeu) {
            skip = 2u;
        } else if (size >= 2u && bytes[0] == 0xfeu && bytes[1] == 0xffu) {
            skip = 2u;
            big_endian = 1;
        }
        return mdict_utf16_to_utf8(bytes + skip, size - skip, big_endian, out);
    }
    {
        const char *source = meta->encoding;
        size_t needed = 0u;
        size_t capacity;
        uint8_t *converted;
        QwCharsetStatus charset_status;
        int status;
        if (strcmp(source, "GBK") == 0 || strcmp(source, "GB2312") == 0 ||
            strcmp(source, "GB18030") == 0)
            source = "GB18030";
        charset_status = qw_charset_convert(source, "UTF-8", bytes, size, NULL,
                                            0u, &needed);
        if (charset_status != QW_CHARSET_OK)
            return mdict_fail(error, error_capacity, QW_MDICT_ERR_UNSUPPORTED,
                              "MDict text encoding is not supported");
        if (!needed) return QW_MDICT_OK;
        if (out->size > out->limit || needed > out->limit - out->size)
            return mdict_fail(error, error_capacity,
                              QW_MDICT_ERR_RESOURCE_LIMIT,
                              "MDict text exceeds the decoded text limit");
        capacity = needed;
        converted = (uint8_t *)malloc(capacity);
        if (!converted)
            return mdict_fail(error, error_capacity,
                              QW_MDICT_ERR_OUT_OF_MEMORY,
                              "out of memory while decoding MDict text");
        charset_status = qw_charset_convert(source, "UTF-8", bytes, size,
                                            converted, capacity, &needed);
        if (charset_status != QW_CHARSET_OK) {
            free(converted);
            return mdict_fail(error, error_capacity,
                              QW_MDICT_ERR_DECODE,
                              "MDict text is not valid in its declared "
                              "encoding");
        }
        status = qw_mdict_buffer_append(out, converted, needed);
        free(converted);
        if (status != QW_MDICT_OK)
            return mdict_fail(error, error_capacity, status,
                              "out of memory while decoding MDict text");
        return QW_MDICT_OK;
    }
}

/* ------------------------------------------------------------------ */
/* Container parsing                                                   */
/* ------------------------------------------------------------------ */

/* Records are sliced by byte offset, so a non-UTF-8 dictionary is converted
 * once here and the offsets are rewritten against the converted stream. */
static int mdict_convert_records(QwMdictContainer *container, char *error,
                                 size_t error_capacity)
{
    QwMdictBuffer converted;
    size_t index;
    int status;

    if (container->resource_file) return QW_MDICT_OK;
    if (mdict_encoding_is_utf8(container->meta.encoding)) return QW_MDICT_OK;
    memset(&converted, 0, sizeof(converted));
    converted.limit = QW_MDICT_MAX_RECORD_BYTES;
    for (index = 0u; index < container->table.count; ++index) {
        uint64_t start = container->table.record_offsets[index];
        uint64_t end = index + 1u < container->table.count
                           ? container->table.record_offsets[index + 1u]
                           : container->records_size;
        size_t new_start = converted.size;
        status = mdict_append_converted(&converted, &container->meta,
                                        container->records + start,
                                        (size_t)(end - start), error,
                                        error_capacity);
        if (status != QW_MDICT_OK) {
            qw_mdict_buffer_free(&converted);
            return status;
        }
        container->table.record_offsets[index] = new_start;
    }
    free(container->records);
    container->records = converted.data;
    container->records_size = converted.size;
    return QW_MDICT_OK;
}

static int mdict_table_reserve(QwMdictTable *table, size_t needed)
{
    uint64_t *offsets;
    size_t *key_offsets;
    size_t *key_lengths;
    size_t capacity;

    if (needed <= table->capacity) return QW_MDICT_OK;
    capacity = table->capacity ? table->capacity : 64u;
    while (capacity < needed) {
        if (capacity > QW_MDICT_MAX_ENTRIES / 2u) {
            capacity = QW_MDICT_MAX_ENTRIES;
            break;
        }
        capacity *= 2u;
    }
    if (capacity < needed || capacity > QW_MDICT_MAX_ENTRIES)
        return QW_MDICT_ERR_RESOURCE_LIMIT;
    offsets = (uint64_t *)realloc(table->record_offsets,
                                  capacity * sizeof(*offsets));
    if (!offsets) return QW_MDICT_ERR_OUT_OF_MEMORY;
    table->record_offsets = offsets;
    key_offsets = (size_t *)realloc(table->key_offsets,
                                    capacity * sizeof(*key_offsets));
    if (!key_offsets) return QW_MDICT_ERR_OUT_OF_MEMORY;
    table->key_offsets = key_offsets;
    key_lengths = (size_t *)realloc(table->key_lengths,
                                    capacity * sizeof(*key_lengths));
    if (!key_lengths) return QW_MDICT_ERR_OUT_OF_MEMORY;
    table->key_lengths = key_lengths;
    table->capacity = capacity;
    return QW_MDICT_OK;
}

static int mdict_table_append(QwMdictTable *table, const QwMdictMeta *meta,
                              const uint8_t *key, size_t key_size,
                              uint64_t record_offset, char *error,
                              size_t error_capacity)
{
    size_t start;
    int status;

    if (table->count >= QW_MDICT_MAX_ENTRIES)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_RESOURCE_LIMIT,
                          "MDict entry count exceeds the supported limit");
    status = mdict_table_reserve(table, table->count + 1u);
    if (status != QW_MDICT_OK)
        return mdict_fail(error, error_capacity, status,
                          "out of memory while growing the MDict key table");
    start = table->key_text.size;
    status = mdict_append_converted(&table->key_text, meta, key, key_size,
                                    error, error_capacity);
    if (status != QW_MDICT_OK) return status;
    if (qw_mdict_buffer_append_byte(&table->key_text, 0u) != QW_MDICT_OK)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_OUT_OF_MEMORY,
                          "out of memory while storing an MDict key");
    table->record_offsets[table->count] = record_offset;
    table->key_offsets[table->count] = start;
    table->key_lengths[table->count] = table->key_text.size - start - 1u;
    table->count += 1u;
    return QW_MDICT_OK;
}

static int mdict_parse_key_block(const uint8_t *block, size_t block_size,
                                 size_t entry_count, const QwMdictMeta *meta,
                                 QwMdictTable *table, char *error,
                                 size_t error_capacity)
{
    int utf16 = mdict_encoding_is_utf16(meta->encoding);
    size_t position = 0u;
    size_t index;

    for (index = 0u; index < entry_count; ++index) {
        uint64_t record_offset;
        size_t key_start;
        size_t key_size;
        int status;

        if (position + 8u > block_size)
            return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                              "MDict key block is truncated");
        record_offset = mdict_load_be64(block + position);
        position += 8u;
        key_start = position;
        if (utf16) {
            while (position + 1u < block_size &&
                   !(block[position] == 0u && block[position + 1u] == 0u))
                position += 2u;
            if (position + 1u >= block_size)
                return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                                  "MDict UTF-16 key is not terminated");
            key_size = position - key_start;
            position += 2u;
        } else {
            const uint8_t *end = (const uint8_t *)memchr(
                block + position, 0, block_size - position);
            if (!end)
                return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                                  "MDict key is not terminated");
            key_size = (size_t)(end - (block + position));
            position = key_start + key_size + 1u;
        }
        if (key_size > QW_MDICT_MAX_KEY_BYTES)
            return mdict_fail(error, error_capacity,
                              QW_MDICT_ERR_RESOURCE_LIMIT,
                              "MDict key exceeds the supported length");
        status = mdict_table_append(table, meta, block + key_start, key_size,
                                    record_offset, error, error_capacity);
        if (status != QW_MDICT_OK) return status;
    }
    return QW_MDICT_OK;
}

typedef struct MdictBlockIndex {
    size_t entries;
    size_t compressed;
    size_t decompressed;
} MdictBlockIndex;

/* The key-block index lists, per block, the entry count, the first and last
 * key (skipped here), and the compressed and decoded block sizes. */
static int mdict_parse_key_info(const uint8_t *info, size_t size,
                                size_t count, int utf16,
                                MdictBlockIndex **out_blocks, char *error,
                                size_t error_capacity)
{
    size_t width = utf16 ? 2u : 1u;
    size_t position = 0u;
    MdictBlockIndex *blocks;
    size_t index;

    if (!count)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict container declares no key blocks");
    if (count > QW_MDICT_MAX_ENTRIES)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_RESOURCE_LIMIT,
                          "MDict key-block count exceeds the supported limit");
    blocks = (MdictBlockIndex *)calloc(count, sizeof(*blocks));
    if (!blocks)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_OUT_OF_MEMORY,
                          "out of memory while reading the key-block index");
    for (index = 0u; index < count; ++index) {
        uint64_t entries;
        uint64_t compressed;
        uint64_t decompressed;
        size_t head_size;
        size_t tail_size;

        if (position + 8u > size) goto truncated;
        entries = mdict_load_be64(info + position);
        position += 8u;
        if (position + 2u > size) goto truncated;
        head_size = mdict_load_be16(info + position);
        position += 2u;
        if ((head_size + 1u) * width > size - position) goto truncated;
        position += (head_size + 1u) * width;
        if (position + 2u > size) goto truncated;
        tail_size = mdict_load_be16(info + position);
        position += 2u;
        if ((tail_size + 1u) * width > size - position) goto truncated;
        position += (tail_size + 1u) * width;
        if (position + 16u > size) goto truncated;
        compressed = mdict_load_be64(info + position);
        position += 8u;
        decompressed = mdict_load_be64(info + position);
        position += 8u;
        if (entries > QW_MDICT_MAX_ENTRIES ||
            compressed > QW_MDICT_MAX_BLOCK_BYTES ||
            decompressed > QW_MDICT_MAX_BLOCK_BYTES ||
            compressed < 8u)
            goto out_of_range;
        blocks[index].entries = (size_t)entries;
        blocks[index].compressed = (size_t)compressed;
        blocks[index].decompressed = (size_t)decompressed;
    }
    *out_blocks = blocks;
    return QW_MDICT_OK;

truncated:
    free(blocks);
    return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                      "MDict key-block index is truncated");
out_of_range:
    free(blocks);
    return mdict_fail(error, error_capacity, QW_MDICT_ERR_RESOURCE_LIMIT,
                      "MDict key-block index declares an out-of-range size");
}

static int mdict_parse_key_blocks(const uint8_t *bytes, size_t size,
                                  size_t offset, size_t section_size,
                                  const MdictBlockIndex *blocks,
                                  size_t block_count, const QwMdictMeta *meta,
                                  QwMdictTable *table, char *error,
                                  size_t error_capacity)
{
    size_t position = offset;
    size_t end;
    size_t index;

    if (section_size > size || offset > size - section_size)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict key blocks run past the end of the file");
    end = offset + section_size;
    for (index = 0u; index < block_count; ++index) {
        uint8_t *decoded = NULL;
        int status;
        size_t consumed = 0u;

        if (blocks[index].compressed > end - position)
            return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                              "MDict key block runs past the key-block "
                              "section");
        status = mdict_decode_block(bytes + position, blocks[index].compressed,
                                    blocks[index].decompressed, &decoded,
                                    error, error_capacity);
        if (status != QW_MDICT_OK) return status;
        consumed = blocks[index].decompressed;
        status = mdict_parse_key_block(decoded, consumed, blocks[index].entries,
                                       meta, table, error, error_capacity);
        free(decoded);
        if (status != QW_MDICT_OK) return status;
        position += blocks[index].compressed;
    }
    if (position != end)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict key-block sizes do not add up to the section");
    return QW_MDICT_OK;
}

/* Read the record section: block table, then the concatenated record bytes. */
static int mdict_parse_records(const uint8_t *bytes, size_t size,
                               size_t offset, QwMdictContainer *container,
                               char *error, size_t error_capacity)
{
    uint64_t block_count;
    uint64_t entry_count;
    uint64_t table_size;
    uint64_t data_size;
    uint64_t total_decoded = 0u;
    uint64_t total_compressed = 0u;
    uint64_t *compressed;
    uint64_t *decoded_sizes;
    size_t index;
    size_t position;
    int status = QW_MDICT_OK;

    if (size - offset < 32u)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict record section is truncated");
    block_count = mdict_load_be64(bytes + offset);
    entry_count = mdict_load_be64(bytes + offset + 8u);
    table_size = mdict_load_be64(bytes + offset + 16u);
    data_size = mdict_load_be64(bytes + offset + 24u);
    if (!block_count ||
        block_count > QW_MDICT_MAX_ENTRIES ||
        table_size != block_count * 16u)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict record block table has an invalid size");
    if (entry_count != container->table.count)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict record entry count does not match the key "
                          "table");
    if (size - offset - 32u < table_size)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                          "MDict record block table is truncated");
    compressed = (uint64_t *)calloc((size_t)block_count, sizeof(*compressed));
    decoded_sizes = (uint64_t *)calloc((size_t)block_count,
                                       sizeof(*decoded_sizes));
    if (!compressed || !decoded_sizes) {
        free(compressed);
        free(decoded_sizes);
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_OUT_OF_MEMORY,
                          "out of memory while reading the record table");
    }
    position = offset + 32u;
    for (index = 0u; index < (size_t)block_count; ++index) {
        compressed[index] = mdict_load_be64(bytes + position);
        decoded_sizes[index] = mdict_load_be64(bytes + position + 8u);
        position += 16u;
        if (compressed[index] < 8u ||
            compressed[index] > QW_MDICT_MAX_BLOCK_BYTES ||
            decoded_sizes[index] > QW_MDICT_MAX_BLOCK_BYTES) {
            status = mdict_fail(error, error_capacity,
                                QW_MDICT_ERR_RESOURCE_LIMIT,
                                "MDict record block size is out of range");
            goto done;
        }
        total_compressed += compressed[index];
        total_decoded += decoded_sizes[index];
        if (total_decoded > QW_MDICT_MAX_RECORD_BYTES) {
            status = mdict_fail(error, error_capacity,
                                QW_MDICT_ERR_RESOURCE_LIMIT,
                                "MDict record stream exceeds the decoded "
                                "record limit");
            goto done;
        }
    }
    if (total_compressed != data_size ||
        data_size > size - position) {
        status = mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                            "MDict record sizes do not match the file");
        goto done;
    }
    if (!total_decoded) {
        status = mdict_fail(error, error_capacity, QW_MDICT_ERR_EMPTY,
                            "MDict container holds no record bytes");
        goto done;
    }
    container->records = (uint8_t *)malloc((size_t)total_decoded);
    if (!container->records) {
        status = mdict_fail(error, error_capacity,
                            QW_MDICT_ERR_OUT_OF_MEMORY,
                            "out of memory while decoding the record stream");
        goto done;
    }
    {
        size_t written = 0u;
        for (index = 0u; index < (size_t)block_count; ++index) {
            uint8_t *block = NULL;
            status = mdict_decode_block(bytes + position,
                                        (size_t)compressed[index],
                                        (size_t)decoded_sizes[index], &block,
                                        error, error_capacity);
            if (status != QW_MDICT_OK) goto done;
            memcpy(container->records + written, block,
                   (size_t)decoded_sizes[index]);
            written += (size_t)decoded_sizes[index];
            free(block);
            position += (size_t)compressed[index];
        }
        container->records_size = written;
    }
    for (index = 0u; index < container->table.count; ++index) {
        if (container->table.record_offsets[index] > container->records_size) {
            status = mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                                "MDict key points past the record stream");
            goto done;
        }
    }
    status = QW_MDICT_OK;

done:
    free(compressed);
    free(decoded_sizes);
    return status;
}

int qw_mdict_open(const void *data, size_t size, int resource_file,
                  QwMdictContainer **out_container, char *error,
                  size_t error_capacity)
{
    const uint8_t *bytes = (const uint8_t *)data;
    QwMdictContainer *container;
    size_t header_end = 0u;
    size_t position;
    uint64_t key_block_count;
    uint64_t entry_count;
    uint64_t info_decoded;
    uint64_t info_size;
    uint64_t key_block_size;
    uint32_t stored_adler;
    uint8_t *info = NULL;
    MdictBlockIndex *blocks = NULL;
    int status;

    if (error && error_capacity) error[0] = '\0';
    if (out_container) *out_container = NULL;
    if (!bytes || !size || !out_container) return QW_MDICT_ERR_INVALID_ARGUMENT;
    if (!qw_mdict_probe_header(data, size, resource_file))
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_NOT_MDICT,
                          "input is not an MDict container");

    container = (QwMdictContainer *)calloc(1u, sizeof(*container));
    if (!container)
        return mdict_fail(error, error_capacity, QW_MDICT_ERR_OUT_OF_MEMORY,
                          "out of memory while opening an MDict container");
    container->resource_file = resource_file;
    container->table.key_text.limit = QW_MDICT_MAX_KEY_BYTES;

    status = mdict_parse_header(bytes, size, resource_file, &container->meta,
                                &header_end, error, error_capacity);
    if (status != QW_MDICT_OK) goto fail;
    if (container->meta.encrypted & 1) {
        status = mdict_fail(error, error_capacity, QW_MDICT_ERR_UNSUPPORTED,
                            "MDict register-code encryption needs the "
                            "registration passcode");
        goto fail;
    }
    if ((uint64_t)header_end + 44u > size) {
        status = mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                            "MDict section table is truncated");
        goto fail;
    }
    position = header_end;
    key_block_count = mdict_load_be64(bytes + position);
    entry_count = mdict_load_be64(bytes + position + 8u);
    info_decoded = mdict_load_be64(bytes + position + 16u);
    info_size = mdict_load_be64(bytes + position + 24u);
    key_block_size = mdict_load_be64(bytes + position + 32u);
    stored_adler = mdict_load_be32(bytes + position + 40u);
    if (stored_adler != qw_compression_adler32(bytes + position, 40u)) {
        status = mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                            "MDict section table checksum does not match");
        goto fail;
    }
    if (!entry_count) {
        status = mdict_fail(error, error_capacity, QW_MDICT_ERR_EMPTY,
                            "MDict container holds no entries");
        goto fail;
    }
    if (entry_count > QW_MDICT_MAX_ENTRIES ||
        info_decoded > QW_MDICT_MAX_INFO_BYTES ||
        info_size > QW_MDICT_MAX_INFO_BYTES ||
        key_block_size > QW_MDICT_MAX_KEY_BYTES) {
        status = mdict_fail(error, error_capacity, QW_MDICT_ERR_RESOURCE_LIMIT,
                            "MDict section sizes exceed the supported limits");
        goto fail;
    }
    container->meta.declared_entries = (size_t)entry_count;
    position += 44u;
    if ((uint64_t)(size - position) < info_size + key_block_size) {
        status = mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                            "MDict key sections run past the end of the file");
        goto fail;
    }

    status = mdict_decode_key_info(bytes + position, (size_t)info_size,
                                   (size_t)info_decoded,
                                   container->meta.encrypted, &info, error,
                                   error_capacity);
    if (status != QW_MDICT_OK) goto fail;
    status = mdict_parse_key_info(info, (size_t)info_decoded,
                                  (size_t)key_block_count,
                                  mdict_encoding_is_utf16(container->meta.encoding),
                                  &blocks, error, error_capacity);
    if (status != QW_MDICT_OK) goto fail;
    free(info);
    info = NULL;

    position += (size_t)info_size;
    status = mdict_parse_key_blocks(bytes, size, position,
                                    (size_t)key_block_size, blocks,
                                    (size_t)key_block_count, &container->meta,
                                    &container->table, error, error_capacity);
    if (status != QW_MDICT_OK) goto fail;
    if (container->table.count != (size_t)entry_count) {
        status = mdict_fail(error, error_capacity, QW_MDICT_ERR_FORMAT,
                            "MDict key count does not match the header");
        goto fail;
    }
    position += (size_t)key_block_size;

    status = mdict_parse_records(bytes, size, position, container, error,
                                 error_capacity);
    if (status != QW_MDICT_OK) goto fail;
    status = mdict_convert_records(container, error, error_capacity);
    if (status != QW_MDICT_OK) goto fail;
    free(blocks);
    *out_container = container;
    return QW_MDICT_OK;

fail:
    free(info);
    free(blocks);
    qw_mdict_close(container);
    return status;
}

void qw_mdict_close(QwMdictContainer *container)
{
    if (!container) return;
    free(container->meta.title);
    free(container->meta.description);
    free(container->meta.style_sheet);
    free(container->table.record_offsets);
    free(container->table.key_offsets);
    free(container->table.key_lengths);
    qw_mdict_buffer_free(&container->table.key_text);
    free(container->records);
    free(container);
}

int qw_mdict_probe_header(const void *data, size_t size, int resource_file)
{
    const uint8_t *bytes = (const uint8_t *)data;
    const char *signature = resource_file ? "<Library_Data" : "<Dictionary";
    size_t length = strlen(signature);
    size_t index;

    if (!bytes || size < 8u) return 0;
    if (mdict_load_be32(bytes) < 8u) return 0;
    if (mdict_load_be32(bytes) > QW_MDICT_MAX_HEADER_BYTES) return 0;
    if ((size_t)mdict_load_be32(bytes) + 8u > size) return 0;
    for (index = 0u; index < length; ++index) {
        size_t offset = 4u + index * 2u;
        if (offset + 1u >= size) return 0;
        if (bytes[offset] != (uint8_t)signature[index] ||
            bytes[offset + 1u] != 0u)
            return 0;
    }
    return 1;
}

const uint8_t *qw_mdict_record_at(const QwMdictContainer *container,
                                  size_t index, size_t *out_size)
{
    uint64_t start;
    uint64_t end;

    if (out_size) *out_size = 0u;
    if (!container || index >= container->table.count) return NULL;
    start = container->table.record_offsets[index];
    end = index + 1u < container->table.count
              ? container->table.record_offsets[index + 1u]
              : container->records_size;
    if (start > container->records_size || end > container->records_size ||
        end < start)
        return NULL;
    if (out_size) *out_size = (size_t)(end - start);
    return container->records + start;
}
