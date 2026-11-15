#include "quirewise/mdx.h"

#include "mdict_internal.h"

#include <quirewise/mdd.h>

#include <stdlib.h>
#include <string.h>

struct QwMdxDocument {
    QwMdictContainer *container;
    uint8_t *html;
    size_t html_size;
    uint8_t *text;
    size_t text_size;
    uint8_t *resource_html;
    size_t resource_html_size;
};

static QwMdxStatus mdx_status_from_container(int status)
{
    switch (status) {
    case QW_MDICT_OK: return QW_MDX_OK;
    case QW_MDICT_ERR_INVALID_ARGUMENT: return QW_MDX_ERROR_INVALID_ARGUMENT;
    case QW_MDICT_ERR_OUT_OF_MEMORY: return QW_MDX_ERROR_OUT_OF_MEMORY;
    case QW_MDICT_ERR_FORMAT: return QW_MDX_ERROR_FORMAT;
    case QW_MDICT_ERR_UNSUPPORTED: return QW_MDX_ERROR_UNSUPPORTED;
    case QW_MDICT_ERR_DECODE: return QW_MDX_ERROR_DECODE;
    case QW_MDICT_ERR_RESOURCE_LIMIT: return QW_MDX_ERROR_RESOURCE_LIMIT;
    case QW_MDICT_ERR_EMPTY: return QW_MDX_ERROR_FORMAT;
    default: return QW_MDX_ERROR_UNKNOWN;
    }
}

int qw_mdx_probe(const void *data, size_t size)
{
    return qw_mdict_probe_header(data, size, 0);
}

QwMdxStatus qw_mdx_parse(const void *data, size_t size,
                         QwMdxDocument **out_document, char *error,
                         size_t error_capacity)
{
    QwMdictContainer *container = NULL;
    QwMdxDocument *document;
    int status;

    if (error && error_capacity) error[0] = '\0';
    if (out_document) *out_document = NULL;
    if (!data || !size || !out_document) return QW_MDX_ERROR_INVALID_ARGUMENT;
    status = qw_mdict_open(data, size, 0, &container, error, error_capacity);
    if (status != QW_MDICT_OK) return mdx_status_from_container(status);
    document = (QwMdxDocument *)calloc(1u, sizeof(*document));
    if (!document) {
        qw_mdict_close(container);
        return QW_MDX_ERROR_OUT_OF_MEMORY;
    }
    document->container = container;
    *out_document = document;
    return QW_MDX_OK;
}

void qw_mdx_document_free(QwMdxDocument *document)
{
    if (!document) return;
    qw_mdict_close(document->container);
    free(document->html);
    free(document->text);
    free(document);
}

const char *qw_mdx_document_title(const QwMdxDocument *document)
{
    if (!document || !document->container) return NULL;
    return document->container->meta.title;
}

const char *qw_mdx_document_description(const QwMdxDocument *document)
{
    if (!document || !document->container) return NULL;
    return document->container->meta.description;
}

const char *qw_mdx_document_encoding(const QwMdxDocument *document)
{
    if (!document || !document->container) return NULL;
    return document->container->meta.encoding;
}

size_t qw_mdx_document_entry_count(const QwMdxDocument *document)
{
    if (!document || !document->container) return 0u;
    return document->container->table.count;
}

QwMdxStatus qw_mdx_document_entry_at(const QwMdxDocument *document,
                                     size_t index, const char **out_key,
                                     size_t *out_key_size,
                                     const char **out_record,
                                     size_t *out_record_size)
{
    const QwMdictTable *table;
    const uint8_t *record;
    size_t record_size = 0u;

    if (out_key) *out_key = NULL;
    if (out_key_size) *out_key_size = 0u;
    if (out_record) *out_record = NULL;
    if (out_record_size) *out_record_size = 0u;
    if (!document || !document->container) return QW_MDX_ERROR_INVALID_ARGUMENT;
    table = &document->container->table;
    if (index >= table->count) return QW_MDX_ERROR_INVALID_ARGUMENT;
    if (out_key)
        *out_key = (const char *)(table->key_text.data + table->key_offsets[index]);
    if (out_key_size) *out_key_size = table->key_lengths[index];
    record = qw_mdict_record_at(document->container, index, &record_size);
    if (!record) return QW_MDX_ERROR_FORMAT;
    while (record_size && record[record_size - 1u] == 0u) --record_size;
    if (out_record) *out_record = (const char *)record;
    if (out_record_size) *out_record_size = record_size;
    return QW_MDX_OK;
}

QwMdxStatus qw_mdx_document_html(QwMdxDocument *document,
                                 const uint8_t **out_html, size_t *out_size)
{
    char error[160];
    int status;

    if (out_html) *out_html = NULL;
    if (out_size) *out_size = 0u;
    if (!document || !document->container || !out_html || !out_size)
        return QW_MDX_ERROR_INVALID_ARGUMENT;
    if (!document->html) {
        error[0] = '\0';
        status = qw_mdict_build_html(document->container, &document->html,
                                     &document->html_size, error,
                                     sizeof(error));
        if (status != QW_MDICT_OK) return mdx_status_from_container(status);
    }
    *out_html = document->html;
    *out_size = document->html_size;
    return QW_MDX_OK;
}

QwMdxStatus qw_mdx_document_html_with_resources(
    QwMdxDocument *document, const QwMddDocument *resources,
    const uint8_t **out_html, size_t *out_size)
{
    char error[160];
    int status;

    if (out_html) *out_html = NULL;
    if (out_size) *out_size = 0u;
    if (!document || !document->container || !out_html || !out_size)
        return QW_MDX_ERROR_INVALID_ARGUMENT;
    if (!resources) {
        return qw_mdx_document_html(document, out_html, out_size);
    }
    if (!document->resource_html) {
        error[0] = '\0';
        status = qw_mdict_build_html_with_resources(
            document->container, resources, &document->resource_html,
            &document->resource_html_size, error, sizeof(error));
        if (status != QW_MDICT_OK) return mdx_status_from_container(status);
    }
    *out_html = document->resource_html;
    *out_size = document->resource_html_size;
    return QW_MDX_OK;
}

QwMdxStatus qw_mdx_document_text(QwMdxDocument *document,
                                 const uint8_t **out_text, size_t *out_size)
{
    char error[160];
    int status;

    if (out_text) *out_text = NULL;
    if (out_size) *out_size = 0u;
    if (!document || !document->container || !out_text || !out_size)
        return QW_MDX_ERROR_INVALID_ARGUMENT;
    if (!document->text) {
        error[0] = '\0';
        status = qw_mdict_build_text(document->container, &document->text,
                                     &document->text_size, error,
                                     sizeof(error));
        if (status != QW_MDICT_OK) return mdx_status_from_container(status);
    }
    *out_text = document->text;
    *out_size = document->text_size;
    return QW_MDX_OK;
}
