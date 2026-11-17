#ifndef QUIREWISE_MOBI_H
#define QUIREWISE_MOBI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QwMobiDocument QwMobiDocument;

typedef enum QwMobiStatus {
    QW_MOBI_OK = 0,
    QW_MOBI_ERROR_INVALID_ARGUMENT = -1,
    QW_MOBI_ERROR_OUT_OF_MEMORY = -2,
    QW_MOBI_ERROR_FORMAT = -4,
    QW_MOBI_ERROR_UNSUPPORTED = -5,
    QW_MOBI_ERROR_DECODE = -6,
    QW_MOBI_ERROR_RESOURCE_LIMIT = -11,
    QW_MOBI_ERROR_UNKNOWN = -100
} QwMobiStatus;

typedef enum QwMobiKind {
    QW_MOBI_KIND_INVALID = 0,
    QW_MOBI_KIND_MOBI,
    QW_MOBI_KIND_AZW3
} QwMobiKind;

/* Parse the supported PalmDOC/MOBI text record into owned UTF-8 text. */
int qw_mobi_probe(const void *data, size_t size);
/*! Parse a MOBI/AZW3 file into a document. */
QwMobiStatus qw_mobi_parse(const void *data, size_t size,
                           QwMobiDocument **out_document);
/*! Return the container kind (MOBI, AZW3, PalmDOC). */
QwMobiKind qw_mobi_document_kind(const QwMobiDocument *document);
/*! Return the extracted text and its size; borrowed until free. */
const uint8_t *qw_mobi_document_text(const QwMobiDocument *document,
                                     size_t *out_size);
/*! Release a document returned by the parser; NULL is accepted. */
void qw_mobi_document_free(QwMobiDocument *document);

#ifdef __cplusplus
}
#endif

#endif
