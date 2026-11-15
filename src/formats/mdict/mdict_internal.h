#ifndef QUIREWISE_MDICT_INTERNAL_H
#define QUIREWISE_MDICT_INTERNAL_H

/*
 * Shared MDict (MDX/MDD) container reader.
 *
 * MDict dictionaries are a two-section container: an index of sorted keys with
 * record offsets, and a concatenated record stream split at those offsets.
 * MdxBuilder 3.x writes engine version 2.0, where every section is a sequence
 * of blocks preceded by an eight-byte header (compression/encryption flags and
 * an Adler-32 checksum of the decoded bytes) and compressed with zlib.
 *
 * The header "Encrypted" field selects the key material:
 *
 *   0  no encryption;
 *   1  the key block is encrypted with a registration passcode;
 *   2  the key-block index and every block are scrambled with a key derived
 *      from each block's own Adler-32, so no passcode is needed;
 *   3  both. The reader supports 0 and 2; 1 and 3 need a registration code the
 *      format does not carry, and engine 1.2 additionally uses LZO blocks.
 *
 * The container reader is bounded by the constants below and reports
 * structural violations as QW_MDICT_ERR_* so both public adapters share one
 * implementation.
 */

#include <stddef.h>
#include <stdint.h>

#define QW_MDICT_MAX_HEADER_BYTES ((size_t)1024u * 1024u)
#define QW_MDICT_MAX_ENTRIES ((size_t)1024u * 1024u)
#define QW_MDICT_MAX_KEY_BYTES ((size_t)64u * 1024u * 1024u)
#define QW_MDICT_MAX_INFO_BYTES ((size_t)32u * 1024u * 1024u)
#define QW_MDICT_MAX_BLOCK_BYTES ((size_t)64u * 1024u * 1024u)
#define QW_MDICT_MAX_RECORD_BYTES ((size_t)192u * 1024u * 1024u)
#define QW_MDICT_MAX_TEXT_BYTES ((size_t)96u * 1024u * 1024u)
#define QW_MDICT_MAX_META_BYTES ((size_t)1024u * 1024u)

enum {
    QW_MDICT_OK = 0,
    QW_MDICT_ERR_INVALID_ARGUMENT = -1,
    QW_MDICT_ERR_OUT_OF_MEMORY = -2,
    QW_MDICT_ERR_IO = -3,
    QW_MDICT_ERR_FORMAT = -4,
    QW_MDICT_ERR_UNSUPPORTED = -5,
    QW_MDICT_ERR_DECODE = -6,
    QW_MDICT_ERR_NOT_MDICT = -7,
    QW_MDICT_ERR_EMPTY = -8,
    QW_MDICT_ERR_RESOURCE_LIMIT = -11
};

/* Growable byte buffer shared by the parser and the renderers. */
typedef struct QwMdictBuffer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t limit;
} QwMdictBuffer;

int qw_mdict_buffer_reserve(QwMdictBuffer *buffer, size_t additional);
int qw_mdict_buffer_append(QwMdictBuffer *buffer, const void *data, size_t size);
int qw_mdict_buffer_append_text(QwMdictBuffer *buffer, const char *text);
int qw_mdict_buffer_append_byte(QwMdictBuffer *buffer, uint8_t value);
void qw_mdict_buffer_clear(QwMdictBuffer *buffer);
void qw_mdict_buffer_free(QwMdictBuffer *buffer);

typedef struct QwMdictMeta {
    char *title;
    char *description;
    char *style_sheet;
    /* Encoding attribute exactly as stored, e.g. "UTF-8", "GBK", "UTF-16". */
    char encoding[24];
    char format[16];
    double engine_version;
    int encrypted;
    int key_case_sensitive;
    int strip_key;
    size_t declared_entries;
} QwMdictMeta;

typedef struct QwMdictTable {
    size_t count;
    size_t capacity;
    uint64_t *record_offsets;
    size_t *key_offsets;
    size_t *key_lengths;
    QwMdictBuffer key_text;
} QwMdictTable;

typedef struct QwMdictContainer {
    QwMdictMeta meta;
    QwMdictTable table;
    uint8_t *records;
    size_t records_size;
    int resource_file;
} QwMdictContainer;

/* Cheap signature check for the MDX ("<Dictionary") or MDD
 * ("<Library_Data") root element in the UTF-16 header. */
int qw_mdict_probe_header(const void *data, size_t size, int resource_file);

/* Parse one whole container. The caller keeps data alive only during the
 * call: every parsed string and the record stream are owned by the result. */
int qw_mdict_open(const void *data, size_t size, int resource_file,
                  QwMdictContainer **out_container, char *error,
                  size_t error_capacity);
void qw_mdict_close(QwMdictContainer *container);

/* Record slice for one entry; returns NULL for an out-of-range index. */
const uint8_t *qw_mdict_record_at(const QwMdictContainer *container,
                                  size_t index, size_t *out_size);

/* Render the whole dictionary as one UTF-8 HTML document; the caller owns the
 * returned buffer and must free it. */
int qw_mdict_build_html_with_resources(const QwMdictContainer *container,
                                       const void *mdd_document,
                                       uint8_t **out_html, size_t *out_size,
                                       char *error, size_t error_capacity);
int qw_mdict_build_html(const QwMdictContainer *container, uint8_t **out_html,
                        size_t *out_size, char *error, size_t error_capacity);

/* Render every entry as UTF-8 key line plus tag-stripped text. */
int qw_mdict_build_text(const QwMdictContainer *container, uint8_t **out_text,
                        size_t *out_size, char *error, size_t error_capacity);

/* Append bytes with HTML metacharacters escaped. */
int qw_mdict_append_escaped(QwMdictBuffer *buffer, const uint8_t *text,
                            size_t size);

/* Append bytes with the XML entities MdxBuilder writes inside attributes and
 * records decoded. */
int qw_mdict_append_unescaped(QwMdictBuffer *buffer, const uint8_t *text,
                              size_t size);

#endif
