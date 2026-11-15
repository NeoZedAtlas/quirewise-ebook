#include "quirewise/mdd.h"

#include "mdict_internal.h"

#include <stdlib.h>
#include <string.h>

struct QwMddDocument {
    QwMdictContainer *container;
};

static QwMddStatus mdd_status_from_container(int status)
{
    switch (status) {
    case QW_MDICT_OK: return QW_MDD_OK;
    case QW_MDICT_ERR_INVALID_ARGUMENT: return QW_MDD_ERROR_INVALID_ARGUMENT;
    case QW_MDICT_ERR_OUT_OF_MEMORY: return QW_MDD_ERROR_OUT_OF_MEMORY;
    case QW_MDICT_ERR_FORMAT: return QW_MDD_ERROR_FORMAT;
    case QW_MDICT_ERR_UNSUPPORTED: return QW_MDD_ERROR_UNSUPPORTED;
    case QW_MDICT_ERR_DECODE: return QW_MDD_ERROR_DECODE;
    case QW_MDICT_ERR_RESOURCE_LIMIT: return QW_MDD_ERROR_RESOURCE_LIMIT;
    case QW_MDICT_ERR_EMPTY: return QW_MDD_ERROR_FORMAT;
    default: return QW_MDD_ERROR_UNKNOWN;
    }
}

int qw_mdd_probe(const void *data, size_t size)
{
    return qw_mdict_probe_header(data, size, 1);
}

QwMddStatus qw_mdd_parse(const void *data, size_t size,
                         QwMddDocument **out_document, char *error,
                         size_t error_capacity)
{
    QwMdictContainer *container = NULL;
    QwMddDocument *document;
    int status;

    if (error && error_capacity) error[0] = '\0';
    if (out_document) *out_document = NULL;
    if (!data || !size || !out_document) return QW_MDD_ERROR_INVALID_ARGUMENT;
    status = qw_mdict_open(data, size, 1, &container, error, error_capacity);
    if (status != QW_MDICT_OK) return mdd_status_from_container(status);
    document = (QwMddDocument *)calloc(1u, sizeof(*document));
    if (!document) {
        qw_mdict_close(container);
        return QW_MDD_ERROR_OUT_OF_MEMORY;
    }
    document->container = container;
    *out_document = document;
    return QW_MDD_OK;
}

void qw_mdd_document_free(QwMddDocument *document)
{
    if (!document) return;
    qw_mdict_close(document->container);
    free(document);
}

const char *qw_mdd_document_title(const QwMddDocument *document)
{
    if (!document || !document->container) return NULL;
    return document->container->meta.title;
}

size_t qw_mdd_document_resource_count(const QwMddDocument *document)
{
    if (!document || !document->container) return 0u;
    return document->container->table.count;
}

QwMddStatus qw_mdd_document_resource_at(const QwMddDocument *document,
                                        size_t index, const char **out_name,
                                        size_t *out_name_size,
                                        const uint8_t **out_data,
                                        size_t *out_data_size)
{
    const QwMdictTable *table;
    const uint8_t *data;
    size_t data_size = 0u;

    if (out_name) *out_name = NULL;
    if (out_name_size) *out_name_size = 0u;
    if (out_data) *out_data = NULL;
    if (out_data_size) *out_data_size = 0u;
    if (!document || !document->container) return QW_MDD_ERROR_INVALID_ARGUMENT;
    table = &document->container->table;
    if (index >= table->count) return QW_MDD_ERROR_INVALID_ARGUMENT;
    if (out_name)
        *out_name =
            (const char *)(table->key_text.data + table->key_offsets[index]);
    if (out_name_size) *out_name_size = table->key_lengths[index];
    data = qw_mdict_record_at(document->container, index, &data_size);
    if (!data) return QW_MDD_ERROR_FORMAT;
    if (out_data) *out_data = data;
    if (out_data_size) *out_data_size = data_size;
    return QW_MDD_OK;
}

QwMddStatus qw_mdd_document_resource_find(const QwMddDocument *document,
                                          const char *name, size_t name_size,
                                          const uint8_t **out_data,
                                          size_t *out_data_size)
{
    const QwMdictTable *table;
    size_t index;

    if (out_data) *out_data = NULL;
    if (out_data_size) *out_data_size = 0u;
    if (!document || !document->container || !name)
        return QW_MDD_ERROR_INVALID_ARGUMENT;
    table = &document->container->table;
    for (index = 0u; index < table->count; ++index) {
        const char *candidate =
            (const char *)(table->key_text.data + table->key_offsets[index]);
        if (table->key_lengths[index] != name_size) continue;
        if (memcmp(candidate, name, name_size) != 0) continue;
        return qw_mdd_document_resource_at(document, index, NULL, NULL,
                                           out_data, out_data_size);
    }
    return QW_MDD_ERROR_INVALID_ARGUMENT;
}
