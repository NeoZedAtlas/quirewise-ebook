#ifndef QUIREWISE_MDD_H
#define QUIREWISE_MDD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MDict resource file (MDD) reader.
 *
 * An MDD shares the MDX container layout but maps UTF-16 resource names such
 * as "\styles.css" or "\00001.gif" to binary payloads referenced by the
 * dictionary articles. The same engine and encryption rules apply.
 */

typedef struct QwMddDocument QwMddDocument;

typedef enum QwMddStatus {
    QW_MDD_OK = 0,
    QW_MDD_ERROR_INVALID_ARGUMENT = -1,
    QW_MDD_ERROR_OUT_OF_MEMORY = -2,
    QW_MDD_ERROR_FORMAT = -4,
    QW_MDD_ERROR_UNSUPPORTED = -5,
    QW_MDD_ERROR_DECODE = -6,
    QW_MDD_ERROR_RESOURCE_LIMIT = -11,
    QW_MDD_ERROR_UNKNOWN = -100
} QwMddStatus;

/* Cheap content probe: the UTF-16 "<Library_Data" root element. */
int qw_mdd_probe(const void *data, size_t size);

/*! Parse an MDD resource archive; error receives a short diagnostic. */
QwMddStatus qw_mdd_parse(const void *data, size_t size,
                         QwMddDocument **out_document, char *error,
                         size_t error_capacity);
/*! Release a document returned by the parser; NULL is accepted. */
void qw_mdd_document_free(QwMddDocument *document);

/*! Return the archive title, or NULL. */
const char *qw_mdd_document_title(const QwMddDocument *document);
/*! Return the number of embedded resources. */
size_t qw_mdd_document_resource_count(const QwMddDocument *document);

/* Borrowed resource name (UTF-8) and payload; both live in the document. */
QwMddStatus qw_mdd_document_resource_at(const QwMddDocument *document,
                                        size_t index, const char **out_name,
                                        size_t *out_name_size,
                                        const uint8_t **out_data,
                                        size_t *out_data_size);

/* Exact-name lookup, e.g. "\00001.gif"; returns INVALID_ARGUMENT when absent. */
QwMddStatus qw_mdd_document_resource_find(const QwMddDocument *document,
                                          const char *name, size_t name_size,
                                          const uint8_t **out_data,
                                          size_t *out_data_size);

#ifdef __cplusplus
}
#endif

#endif
