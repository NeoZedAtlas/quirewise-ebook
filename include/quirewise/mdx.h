#ifndef QUIREWISE_MDX_H
#define QUIREWISE_MDX_H

#include "quirewise/mdd.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MDict dictionary (MDX) reader.
 *
 * MDX packages a headword-to-article map: an index of sorted keys with record
 * offsets plus a record stream that holds one HTML or text article per key.
 * The companion MDD resource file is read by quirewise/mdd.h.
 *
 * Supported: engine 2.0 containers (MdxBuilder 3.x), zlib or stored blocks,
 * UTF-8, UTF-16 and the legacy CJK code pages the charset package knows, and
 * the "Encrypted=2" block scrambling that derives each block key from its own
 * checksum. Engine 1.2 (LZO) and register-code encrypted dictionaries
 * ("Encrypted=1" or "3") report QW_MDX_ERROR_UNSUPPORTED.
 */

typedef struct QwMdxDocument QwMdxDocument;

typedef enum QwMdxStatus {
    QW_MDX_OK = 0,
    QW_MDX_ERROR_INVALID_ARGUMENT = -1,
    QW_MDX_ERROR_OUT_OF_MEMORY = -2,
    QW_MDX_ERROR_FORMAT = -4,
    QW_MDX_ERROR_UNSUPPORTED = -5,
    QW_MDX_ERROR_DECODE = -6,
    QW_MDX_ERROR_RESOURCE_LIMIT = -11,
    QW_MDX_ERROR_UNKNOWN = -100
} QwMdxStatus;

/*
 * Cheap content probe: the UTF-16 "<Dictionary" root element behind a
 * big-endian header length. It intentionally does not validate the index.
 */
int qw_mdx_probe(const void *data, size_t size);

/*
 * Parse one dictionary. Keys and records are returned as UTF-8; every pointer
 * the document exposes stays valid until qw_mdx_document_free. On failure the
 * optional error buffer receives a short diagnostic.
 */
QwMdxStatus qw_mdx_parse(const void *data, size_t size,
                         QwMdxDocument **out_document, char *error,
                         size_t error_capacity);
void qw_mdx_document_free(QwMdxDocument *document);

/* Header metadata; strings may be NULL and are owned by the document. */
const char *qw_mdx_document_title(const QwMdxDocument *document);
const char *qw_mdx_document_description(const QwMdxDocument *document);
const char *qw_mdx_document_encoding(const QwMdxDocument *document);
size_t qw_mdx_document_entry_count(const QwMdxDocument *document);

/* One entry; out pointers are borrowed and remain valid until free. */
QwMdxStatus qw_mdx_document_entry_at(const QwMdxDocument *document,
                                     size_t index, const char **out_key,
                                     size_t *out_key_size,
                                     const char **out_record,
                                     size_t *out_record_size);

/*
 * Whole-dictionary rendering, built on first use and cached in the document:
 * rhtml is one UTF-8 HTML document (title, description, one heading and body
 * per entry) for the layout/PDF route; text is the same content with block
 * markup reduced to line breaks. The caller must not free either buffer.
 */
QwMdxStatus qw_mdx_document_html(QwMdxDocument *document,
                                 const uint8_t **out_html,
                                 size_t *out_size);
/*
 * Same as qw_mdx_document_html(), but <img src> references are resolved
 * through a companion MDD document and replaced with data URIs. Passing NULL
 * behaves like qw_mdx_document_html(). The returned buffer is cached in the
 * MDX document and must not be freed by the caller.
 */
QwMdxStatus qw_mdx_document_html_with_resources(
    QwMdxDocument *document, const QwMddDocument *resources,
    const uint8_t **out_html, size_t *out_size);

QwMdxStatus qw_mdx_document_text(QwMdxDocument *document,
                                 const uint8_t **out_text, size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif
