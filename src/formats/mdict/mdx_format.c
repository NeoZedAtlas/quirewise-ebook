#include "../../core/format_registry.h"
#include "mdict_internal.h"

#include "quirewise/mdx.h"

#include <stdio.h>
#include <string.h>

static int mdx_probe_descriptor(const void *data, size_t size)
{
    return qw_mdx_probe(data, size);
}

static void mdx_layout_dispose(void *context)
{
    qw_mdx_document_free((QwMdxDocument *)context);
}

static int mdx_status_to_ebook(QwMdxStatus status)
{
    switch (status) {
    case QW_MDX_OK: return QW_EBOOK_OK;
    case QW_MDX_ERROR_INVALID_ARGUMENT: return QW_EBOOK_ERR_INVALID_ARGUMENT;
    case QW_MDX_ERROR_OUT_OF_MEMORY: return QW_EBOOK_ERR_OUT_OF_MEMORY;
    case QW_MDX_ERROR_FORMAT: return QW_EBOOK_ERR_FORMAT;
    case QW_MDX_ERROR_UNSUPPORTED: return QW_EBOOK_ERR_UNSUPPORTED;
    case QW_MDX_ERROR_DECODE: return QW_EBOOK_ERR_DECODE;
    case QW_MDX_ERROR_RESOURCE_LIMIT: return QW_EBOOK_ERR_RESOURCE_LIMIT;
    default: return QW_EBOOK_ERR_FORMAT;
    }
}

/* The registry view of an MDX input is the shared HTML-backed layout: the
 * document package owns PDF emission, this package owns the dictionary. */
static int mdx_open_layout(const void *data, size_t size,
                           QwInputFormat input_format,
                           const QwConvertOptions *options,
                           QwLayoutDocument *out_document,
                           QwConvertReport *report)
{
    QwMdxDocument *document = NULL;
    const uint8_t *html = NULL;
    size_t html_size = 0u;
    char error[160];
    QwMdxStatus status;

    (void)options;
    if (!out_document) return QW_EBOOK_ERR_INVALID_ARGUMENT;
    memset(out_document, 0, sizeof(*out_document));
    if (!data || !size || input_format != QW_FORMAT_MDX)
        return QW_EBOOK_ERR_INVALID_ARGUMENT;
    error[0] = '\0';
    status = qw_mdx_parse(data, size, &document, error, sizeof(error));
    if (status != QW_MDX_OK) {
        if (report) {
            snprintf(report->note, sizeof(report->note), "MDX: %s",
                     error[0] ? error : "dictionary could not be parsed");
        }
        return mdx_status_to_ebook(status);
    }
    status = qw_mdx_document_html(document, &html, &html_size);
    if (status != QW_MDX_OK) {
        qw_mdx_document_free(document);
        return mdx_status_to_ebook(status);
    }
    out_document->html_utf8 = html;
    out_document->html_size = html_size;
    out_document->context = document;
    out_document->dispose = mdx_layout_dispose;
    if (report) {
        snprintf(report->note, sizeof(report->note),
                 "MDict dictionary rendered through the shared HTML layout; "
                 "MDD image resources are not embedded");
    }
    return QW_EBOOK_OK;
}

const QwFormatDescriptor qw_format_mdx = {
    .id = QW_FORMAT_MDX,
    .name = "MDict dictionary",
    .capabilities = (QW_FORMAT_CAP_MULTI_PAGE),
    .probe = mdx_probe_descriptor,
    .open_memory = NULL,
    .open_layout = mdx_open_layout,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = NULL,
    .open_spreadsheet = NULL,
    .open_subtitle = NULL,
    .open_data = NULL
};
