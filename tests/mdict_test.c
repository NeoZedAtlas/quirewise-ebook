/*
 * MDict reader tests.
 *
 * The fixtures are built in memory instead of being committed as binaries:
 * the writer below emits the engine-2.0 layout (header, section table, key
 * index, key blocks, record blocks), once unencrypted and once with the
 * "Encrypted=2" block scrambling the format uses, so both decode paths run.
 */

#include <quirewise/compression.h>
#include <quirewise/crypto.h>
#include <quirewise/mdd.h>
#include <quirewise/mdx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TestBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
} TestBuffer;

typedef struct FixtureEntry {
    const char *key;
    const char *record;
} FixtureEntry;

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static int buffer_reserve(TestBuffer *buffer, size_t additional)
{
    size_t needed;
    size_t capacity;
    uint8_t *grown;

    if (buffer->size > SIZE_MAX - additional) return 0;
    needed = buffer->size + additional;
    if (needed <= buffer->capacity) return 1;
    capacity = buffer->capacity ? buffer->capacity : 128u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) return 0;
        capacity *= 2u;
    }
    grown = (uint8_t *)realloc(buffer->data, capacity);
    if (!grown) return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static void buffer_append(TestBuffer *buffer, const void *data, size_t size)
{
    if (!size) return;
    if (!buffer_reserve(buffer, size)) fail("fixture buffer overflow");
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
}

static void buffer_u8(TestBuffer *buffer, uint8_t value)
{
    buffer_append(buffer, &value, 1u);
}

static void buffer_u16be(TestBuffer *buffer, uint16_t value)
{
    uint8_t bytes[2];
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
    buffer_append(buffer, bytes, sizeof(bytes));
}

static void buffer_u32be(TestBuffer *buffer, uint32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
    buffer_append(buffer, bytes, sizeof(bytes));
}

static void buffer_u32le(TestBuffer *buffer, uint32_t value)
{
    uint8_t bytes[4];
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
    buffer_append(buffer, bytes, sizeof(bytes));
}

static void buffer_u64be(TestBuffer *buffer, uint64_t value)
{
    buffer_u32be(buffer, (uint32_t)(value >> 32));
    buffer_u32be(buffer, (uint32_t)value);
}

static void buffer_utf16le(TestBuffer *buffer, const char *text)
{
    size_t index;
    for (index = 0u; text[index]; ++index) {
        uint16_t unit = (uint16_t)(unsigned char)text[index];
        uint8_t bytes[2];
        bytes[0] = (uint8_t)unit;
        bytes[1] = (uint8_t)(unit >> 8);
        buffer_append(buffer, bytes, sizeof(bytes));
    }
}

/* The format's block cipher inverse, so the test can build a scrambled file. */
static void fast_encrypt(uint8_t *data, size_t size, const uint8_t key[16])
{
    uint8_t previous = 0x36u;
    size_t index;

    for (index = 0u; index < size; ++index) {
        uint8_t value =
            (uint8_t)(data[index] ^ previous ^ (uint8_t)index ^ key[index % 16u]);
        value = (uint8_t)((value >> 4) | (value << 4));
        previous = value;
        data[index] = value;
    }
}

static uint8_t *zlib_stored(const uint8_t *input, size_t input_size,
                            size_t *out_size)
{
    size_t bound = qw_compression_zlib_stored_bound(input_size);
    uint8_t *output = (uint8_t *)malloc(bound ? bound : 1u);
    size_t written = 0u;

    if (!output) fail("out of memory for a compressed fixture");
    if (!qw_compression_deflate_zlib_stored(input, input_size, output, bound,
                                            &written))
        fail("zlib stored compression failed");
    *out_size = written;
    return output;
}

/* Append one block: descriptor, Adler-32 of the decoded bytes, payload. When
 * `encrypted` is set the payload is scrambled with the checksum-derived key. */
static void append_block(TestBuffer *out, const uint8_t *content,
                         size_t content_size, int with_encryption_bits,
                         int encrypted)
{
    size_t payload_size = 0u;
    uint8_t *payload = zlib_stored(content, content_size, &payload_size);
    uint32_t adler = qw_compression_adler32(content, content_size);
    uint32_t descriptor = 2u;

    if (with_encryption_bits && encrypted) {
        descriptor |= 1u << 4;
        descriptor |= (uint32_t)(payload_size & 0xffu) << 8;
    }
    buffer_u32le(out, descriptor);
    buffer_u32be(out, adler);
    if (with_encryption_bits && encrypted) {
        uint8_t seed[4];
        uint8_t key[16];
        seed[0] = (uint8_t)(adler >> 24);
        seed[1] = (uint8_t)(adler >> 16);
        seed[2] = (uint8_t)(adler >> 8);
        seed[3] = (uint8_t)adler;
        if (qw_crypto_ripemd128(seed, sizeof(seed), key) != QW_CRYPTO_OK)
            fail("block key derivation failed");
        fast_encrypt(payload, payload_size, key);
    }
    buffer_append(out, payload, payload_size);
    free(payload);
}


static void build_container(int resource_file, int encrypted,
                            const FixtureEntry *entries, size_t entry_count,
                            TestBuffer *out)
{
    TestBuffer header_text;
    TestBuffer records;
    TestBuffer key_block;
    TestBuffer key_info;
    TestBuffer compressed_info;
    size_t *offsets;
    size_t index;
    size_t info_size = 0u;
    uint32_t header_adler;
    size_t key_block_size;
    size_t record_block_size;
    uint32_t numbers_adler;

    memset(&header_text, 0, sizeof(header_text));
    memset(&records, 0, sizeof(records));
    memset(&key_block, 0, sizeof(key_block));
    memset(&key_info, 0, sizeof(key_info));
    memset(&compressed_info, 0, sizeof(compressed_info));
    memset(out, 0, sizeof(*out));

    offsets = (size_t *)calloc(entry_count, sizeof(*offsets));
    if (!offsets) fail("out of memory for fixture offsets");

    /* Record stream with one offset per entry. */
    for (index = 0u; index < entry_count; ++index) {
        offsets[index] = records.size;
        buffer_append(&records, entries[index].record,
                      strlen(entries[index].record));
    }

    /* Key block: 64-bit record offset, key text, terminator. */
    for (index = 0u; index < entry_count; ++index) {
        buffer_u64be(&key_block, offsets[index]);
        if (resource_file) {
            buffer_utf16le(&key_block, entries[index].key);
            buffer_u16be(&key_block, 0u);
        } else {
            buffer_append(&key_block, entries[index].key,
                          strlen(entries[index].key));
            buffer_u8(&key_block, 0u);
        }
    }

    /* Key-block index: one block, first/last key plus sizes. */
    buffer_u64be(&key_info, entry_count);
    buffer_u16be(&key_info, (uint16_t)strlen(entries[0].key));
    if (resource_file) {
        buffer_utf16le(&key_info, entries[0].key);
        buffer_u16be(&key_info, 0u);
        buffer_u16be(&key_info, (uint16_t)strlen(entries[entry_count - 1u].key));
        buffer_utf16le(&key_info, entries[entry_count - 1u].key);
        buffer_u16be(&key_info, 0u);
    } else {
        buffer_append(&key_info, entries[0].key, strlen(entries[0].key));
        buffer_u8(&key_info, 0u);
        buffer_u16be(&key_info,
                     (uint16_t)strlen(entries[entry_count - 1u].key));
        buffer_append(&key_info, entries[entry_count - 1u].key,
                      strlen(entries[entry_count - 1u].key));
        buffer_u8(&key_info, 0u);
    }
    {
        size_t compressed_size = 0u;
        uint8_t *compressed = zlib_stored(key_block.data, key_block.size,
                                          &compressed_size);
        buffer_u64be(&key_info, compressed_size + 8u);
        free(compressed);
    }
    buffer_u64be(&key_info, key_block.size);

    {
        uint32_t adler = qw_compression_adler32(key_info.data, key_info.size);
        uint8_t *compressed = zlib_stored(key_info.data, key_info.size,
                                          &info_size);
        buffer_u32le(&compressed_info, 2u);
        buffer_u32be(&compressed_info, adler);
        if (encrypted) {
            uint8_t seed[8];
            uint8_t key[16];
            seed[0] = (uint8_t)(adler >> 24);
            seed[1] = (uint8_t)(adler >> 16);
            seed[2] = (uint8_t)(adler >> 8);
            seed[3] = (uint8_t)adler;
            seed[4] = 0x95u;
            seed[5] = 0x36u;
            seed[6] = 0x00u;
            seed[7] = 0x00u;
            if (qw_crypto_ripemd128(seed, sizeof(seed), key) != QW_CRYPTO_OK)
                fail("key-index key derivation failed");
            fast_encrypt(compressed, info_size, key);
        }
        buffer_append(&compressed_info, compressed, info_size);
        free(compressed);
    }

    /* Header element in UTF-16LE without a byte-order mark. */
    buffer_utf16le(&header_text, resource_file ? "<Library_Data " : "<Dictionary ");
    buffer_utf16le(&header_text, "GeneratedByEngineVersion=\"2.0\" ");
    buffer_utf16le(&header_text, encrypted ? "Encrypted=\"2\" " : "Encrypted=\"0\" ");
    if (!resource_file) {
        buffer_utf16le(&header_text, "Encoding=\"UTF-8\" Format=\"Html\" ");
        buffer_utf16le(&header_text, "KeyCaseSensitive=\"No\" StripKey=\"Yes\" ");
    }
    buffer_utf16le(&header_text, "Title=\"Fixture\" ");
    buffer_utf16le(&header_text, "Description=\"Synthetic entries\"");
    buffer_utf16le(&header_text, resource_file ? "/>" : "/>");

    buffer_u32be(out, (uint32_t)header_text.size);
    buffer_append(out, header_text.data, header_text.size);
    header_adler = qw_compression_adler32(header_text.data, header_text.size);
    {
        uint8_t bytes[4];
        bytes[0] = (uint8_t)header_adler;
        bytes[1] = (uint8_t)(header_adler >> 8);
        bytes[2] = (uint8_t)(header_adler >> 16);
        bytes[3] = (uint8_t)(header_adler >> 24);
        buffer_append(out, bytes, sizeof(bytes));
    }

    /* Section table: one key block, one record block. */
    key_block_size = 8u;
    {
        size_t probe_size = 0u;
        uint8_t *probe = zlib_stored(key_block.data, key_block.size,
                                     &probe_size);
        key_block_size = probe_size + 8u;
        free(probe);
    }
    {
        size_t probe_size = 0u;
        uint8_t *probe = zlib_stored(records.data, records.size, &probe_size);
        record_block_size = probe_size + 8u;
        free(probe);
    }
    {
        TestBuffer numbers;
        memset(&numbers, 0, sizeof(numbers));
        buffer_u64be(&numbers, 1u);
        buffer_u64be(&numbers, entry_count);
        buffer_u64be(&numbers, key_info.size);
        buffer_u64be(&numbers, compressed_info.size);
        buffer_u64be(&numbers, key_block_size);
        numbers_adler = qw_compression_adler32(numbers.data, numbers.size);
        buffer_append(out, numbers.data, numbers.size);
        buffer_u32be(out, numbers_adler);
        free(numbers.data);
    }
    buffer_append(out, compressed_info.data, compressed_info.size);
    append_block(out, key_block.data, key_block.size, 1, encrypted);
    {
        TestBuffer record_section;
        memset(&record_section, 0, sizeof(record_section));
        buffer_u64be(&record_section, 1u);
        buffer_u64be(&record_section, entry_count);
        buffer_u64be(&record_section, 16u);
        buffer_u64be(&record_section, record_block_size);
        buffer_u64be(&record_section, record_block_size);
        buffer_u64be(&record_section, records.size);
        buffer_append(out, record_section.data, record_section.size);
        free(record_section.data);
    }
    append_block(out, records.data, records.size, 1, encrypted);

    free(offsets);
    free(header_text.data);
    free(records.data);
    free(key_block.data);
    free(key_info.data);
    free(compressed_info.data);
}

static int memmem_contains(const uint8_t *haystack, size_t haystack_size,
                           const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;

    if (needle_size > haystack_size) return 0;
    for (index = 0u; index + needle_size <= haystack_size; ++index) {
        if (memcmp(haystack + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}
static void check_mdx(const uint8_t *data, size_t size, int encrypted)
{
    QwMdxDocument *document = NULL;
    char error[192];
    const char *key = NULL;
    const char *record = NULL;
    size_t key_size = 0u;
    size_t record_size = 0u;

    memset(error, 0, sizeof(error));
    if (!qw_mdx_probe(data, size)) fail("MDX signature probe missed");
    if (qw_mdx_probe(data, size / 2u) && size > 64u)
        fail("MDX probe accepted a truncated header");
    if (qw_mdx_parse(data, size, &document, error, sizeof(error)) != QW_MDX_OK) {
        fprintf(stderr, "MDX parse failed: %s\n", error);
        fail("valid MDX did not parse");
    }
    if (qw_mdx_document_entry_count(document) != 3u)
        fail("MDX entry count mismatch");
    if (strcmp(qw_mdx_document_encoding(document), "UTF-8") != 0)
        fail("MDX encoding mismatch");
    if (!qw_mdx_document_title(document) ||
        strcmp(qw_mdx_document_title(document), "Fixture") != 0)
        fail("MDX title mismatch");
    if (qw_mdx_document_entry_at(document, 0u, &key, &key_size, &record,
                                 &record_size) != QW_MDX_OK)
        fail("MDX entry 0 missing");
    if (key_size != 5u || memcmp(key, "alpha", 5u) != 0)
        fail("MDX key 0 mismatch");
    if (record_size != strlen("<p>Alpha definition</p>") ||
        memcmp(record, "<p>Alpha definition</p>", record_size) != 0)
        fail("MDX record 0 mismatch");
    if (qw_mdx_document_entry_at(document, 2u, &key, &key_size, NULL, NULL) !=
        QW_MDX_OK)
        fail("MDX entry 2 missing");
    if (key_size != strlen("中文键") || memcmp(key, "中文键", key_size) != 0)
        fail("MDX UTF-8 key mismatch");
    if (qw_mdx_document_entry_at(document, 3u, NULL, NULL, NULL, NULL) !=
        QW_MDX_ERROR_INVALID_ARGUMENT)
        fail("MDX out-of-range entry accepted");
    {
        const uint8_t *html = NULL;
        size_t html_size = 0u;
        if (qw_mdx_document_html(document, &html, &html_size) != QW_MDX_OK ||
            !html_size || !memmem_contains(html, html_size, "Alpha definition"))
            fail("MDX HTML rendering lost the body");
        if (!memmem_contains(html, html_size, "beta"))
            fail("MDX HTML rendering lost a key");
    }
    {
        const uint8_t *text = NULL;
        size_t text_size = 0u;
        if (qw_mdx_document_text(document, &text, &text_size) != QW_MDX_OK ||
            !text_size)
            fail("MDX text rendering failed");
        if (!memmem_contains(text, text_size, "Alpha definition"))
            fail("MDX text rendering lost the body");
        if (memmem_contains(text, text_size, "<p>"))
            fail("MDX text rendering kept block markup");
    }
    qw_mdx_document_free(document);
    (void)encrypted;
}


static void check_mdd(const uint8_t *data, size_t size)
{
    QwMddDocument *document = NULL;
    char error[192];
    const char *name = NULL;
    const uint8_t *payload = NULL;
    size_t name_size = 0u;
    size_t payload_size = 0u;

    memset(error, 0, sizeof(error));
    if (!qw_mdd_probe(data, size)) fail("MDD signature probe missed");
    if (qw_mdx_probe(data, size)) fail("MDD data matched the MDX probe");
    if (qw_mdd_parse(data, size, &document, error, sizeof(error)) != QW_MDD_OK) {
        fprintf(stderr, "MDD parse failed: %s\n", error);
        fail("valid MDD did not parse");
    }
    if (qw_mdd_document_resource_count(document) != 2u)
        fail("MDD resource count mismatch");
    if (qw_mdd_document_resource_at(document, 0u, &name, &name_size, &payload,
                                    &payload_size) != QW_MDD_OK)
        fail("MDD resource 0 missing");
    if (name_size != strlen("\\00001.gif") ||
        memcmp(name, "\\00001.gif", name_size) != 0)
        fail("MDD resource name mismatch");
    if (payload_size != 6u || memcmp(payload, "GIF89a", 6u) != 0)
        fail("MDD resource payload mismatch");
    if (qw_mdd_document_resource_find(document, "\\00001.gif", 10u, &payload,
                                      &payload_size) != QW_MDD_OK ||
        payload_size != 6u)
        fail("MDD resource lookup failed");
    if (qw_mdd_document_resource_find(document, "\\missing", 8u, &payload,
                                      &payload_size) !=
        QW_MDD_ERROR_INVALID_ARGUMENT)
        fail("MDD missing resource did not report an error");
    qw_mdd_document_free(document);
}

static void check_rejections(const uint8_t *data, size_t size)
{
    QwMdxDocument *document = NULL;
    char error[192];
    size_t cut;

    for (cut = 1u; cut < size; cut += 37u) {
        memset(error, 0, sizeof(error));
        document = NULL;
        if (qw_mdx_parse(data, cut, &document, error, sizeof(error)) ==
            QW_MDX_OK) {
            fprintf(stderr, "truncated MDX at %zu bytes parsed as valid\n", cut);
            fail("truncated MDX accepted");
        }
        qw_mdx_document_free(document);
    }
}

/* Deterministic byte flips: the parsers must reject or accept them without
 * reading out of bounds, so a surviving run is the assertion. */
static void check_mutations(const uint8_t *data, size_t size, int resource_file)
{
    uint8_t *buffer;
    uint32_t state = 0x51f0dd99u;
    size_t run;

    buffer = (uint8_t *)malloc(size ? size : 1u);
    if (!buffer) fail("out of memory for mutation runs");
    for (run = 0u; run < 120u; ++run) {
        size_t length = size;
        unsigned int flips;
        unsigned int flip;
        QwMdxDocument *mdx = NULL;
        QwMddDocument *mdd = NULL;
        char error[192];

        memcpy(buffer, data, size);
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        flips = 1u + state % 6u;
        for (flip = 0u; flip < flips; ++flip) {
            state ^= state << 13u;
            state ^= state >> 17u;
            state ^= state << 5u;
            buffer[state % length] ^= (uint8_t)(1u << (state % 8u));
        }
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        if (state % 5u == 0u) length = 1u + state % length;
        memset(error, 0, sizeof(error));
        if (resource_file) {
            if (qw_mdd_parse(buffer, length, &mdd, error, sizeof(error)) ==
                QW_MDD_OK)
                qw_mdd_document_free(mdd);
        } else if (qw_mdx_parse(buffer, length, &mdx, error, sizeof(error)) ==
                   QW_MDX_OK) {
            qw_mdx_document_free(mdx);
        }
    }
    free(buffer);
}

int main(void)
{
    static const FixtureEntry mdx_entries[] = {
        { "alpha", "<p>Alpha definition</p>" },
        { "beta", "<p>Beta<br>second line</p>" },
        { "中文键", "<p>Chinese key definition</p>" }
    };
    static const FixtureEntry mdd_entries[] = {
        { "\\00001.gif", "GIF89a" },
        { "\\style.css", "body{}" }
    };
    TestBuffer plain;
    TestBuffer scrambled;
    TestBuffer resources;

    build_container(0, 0, mdx_entries, 3u, &plain);
    check_mdx(plain.data, plain.size, 0);
    check_rejections(plain.data, plain.size);
    check_mutations(plain.data, plain.size, 0);

    build_container(0, 1, mdx_entries, 3u, &scrambled);
    check_mdx(scrambled.data, scrambled.size, 1);
    check_mutations(scrambled.data, scrambled.size, 0);

    build_container(1, 0, mdd_entries, 2u, &resources);
    check_mdd(resources.data, resources.size);
    check_mutations(resources.data, resources.size, 1);

    {
        /* A single flipped header checksum byte must be rejected. */
        if (scrambled.size > 8u) {
            uint8_t saved = scrambled.data[7];
            QwMdxDocument *document = NULL;
            char error[192];
            scrambled.data[7] = (uint8_t)(saved ^ 0x5au);
            memset(error, 0, sizeof(error));
            if (qw_mdx_parse(scrambled.data, scrambled.size, &document, error,
                             sizeof(error)) == QW_MDX_OK) {
                qw_mdx_document_free(document);
                fail("corrupt header checksum accepted");
            }
            scrambled.data[7] = saved;
        }
    }

    free(plain.data);
    free(scrambled.data);
    free(resources.data);
    puts("Quirewise MDict reader tests passed");
    return 0;
}
