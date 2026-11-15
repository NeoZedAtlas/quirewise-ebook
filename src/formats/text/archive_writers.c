/* Text-archive and HTMLZ writers.
 *
 * Both formats are ZIP32 containers around one primary text member:
 * TXTZ -> index.txt, PMLZ -> index.pml, HTMLZ -> index.html. The internal ZIP
 * writer from the archive package is shared here through the pinned private
 * header, exactly like the readers do. Output is a flat, uncompressed archive
 * with the primary member at a fixed name, which is what the readers select.
 */

#include "quirewise/htmlz.h"
#include "quirewise/text_archive.h"

#include "../archive/zip_native.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define QW_ARCHIVE_WRITE_LIMIT ((size_t)64u * 1024u * 1024u)

static int archive_build_single(const char *member_name, const void *data,
                                size_t size, uint8_t **out_archive,
                                size_t *out_size)
{
    QwZipWriter writer;
    void *archive = NULL;
    size_t archive_size = 0u;

    if (!member_name || !data || !size || !out_archive || !out_size ||
        size > QW_ARCHIVE_WRITE_LIMIT)
        return 0;
    memset(&writer, 0, sizeof(writer));
    if (!qw_zip_writer_init_heap(&writer, 0u, 0u)) return 0;
    if (!qw_zip_writer_add_mem(&writer, member_name, data, size,
                               QW_ZIP_NO_COMPRESSION) ||
        !qw_zip_writer_finalize_heap_archive(&writer, &archive,
                                             &archive_size) ||
        !archive) {
        qw_zip_writer_end(&writer);
        return 0;
    }
    qw_zip_writer_end(&writer);
    *out_archive = (uint8_t *)archive;
    *out_size = archive_size;
    return 1;
}

QwTextArchiveStatus qw_text_archive_create(QwTextArchiveKind kind,
                                           const void *text, size_t text_size,
                                           uint8_t **out_archive,
                                           size_t *out_size)
{
    const char *member;

    if (out_archive) *out_archive = NULL;
    if (out_size) *out_size = 0u;
    if (!text || !text_size || !out_archive || !out_size)
        return QW_TEXT_ARCHIVE_ERR_INVALID_ARGUMENT;
    if (text_size > QW_ARCHIVE_WRITE_LIMIT)
        return QW_TEXT_ARCHIVE_ERR_RESOURCE_LIMIT;
    if (kind == QW_TEXT_ARCHIVE_TXTZ) member = "index.txt";
    else if (kind == QW_TEXT_ARCHIVE_PMLZ) member = "index.pml";
    else return QW_TEXT_ARCHIVE_ERR_INVALID_ARGUMENT;
    if (!archive_build_single(member, text, text_size, out_archive, out_size)) {
        if (out_archive) *out_archive = NULL;
        if (out_size) *out_size = 0u;
        return QW_TEXT_ARCHIVE_ERR_OUT_OF_MEMORY;
    }
    return QW_TEXT_ARCHIVE_OK;
}

void qw_text_archive_buffer_free(void *buffer)
{
    free(buffer);
}

static int archive_contains(const void *data, size_t size,
                            const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;

    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp((const uint8_t *)data + index, needle, needle_size) == 0)
            return 1;
    }
    return 0;
}

QwHtmlzStatus qw_htmlz_create(const void *html, size_t html_size,
                              uint8_t **out_archive, size_t *out_size)
{
    static const char prefix[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"></head><body>\n";
    static const char suffix[] = "\n</body></html>\n";
    const uint8_t *payload = (const uint8_t *)html;
    size_t payload_size = html_size;
    uint8_t *wrapped = NULL;

    if (out_archive) *out_archive = NULL;
    if (out_size) *out_size = 0u;
    if (!html || !html_size || !out_archive || !out_size)
        return QW_HTMLZ_ERR_INVALID_ARGUMENT;
    if (html_size > QW_ARCHIVE_WRITE_LIMIT)
        return QW_HTMLZ_ERR_RESOURCE_LIMIT;
    /* The reader only selects a member with an HTML root/doctype marker, so a
     * fragment (the layout callers often pass one) is wrapped into a complete
     * document. */
    if (!archive_contains(html, html_size, "<html") &&
        !archive_contains(html, html_size, "<!doctype") &&
        !archive_contains(html, html_size, "<!DOCTYPE")) {
        size_t prefix_size = strlen(prefix);
        size_t suffix_size = strlen(suffix);
        if (html_size > QW_ARCHIVE_WRITE_LIMIT - prefix_size - suffix_size)
            return QW_HTMLZ_ERR_RESOURCE_LIMIT;
        wrapped = (uint8_t *)malloc(prefix_size + html_size + suffix_size);
        if (!wrapped) return QW_HTMLZ_ERR_OUT_OF_MEMORY;
        memcpy(wrapped, prefix, prefix_size);
        memcpy(wrapped + prefix_size, html, html_size);
        memcpy(wrapped + prefix_size + html_size, suffix, suffix_size);
        payload = wrapped;
        payload_size = prefix_size + html_size + suffix_size;
    }
    if (!archive_build_single("index.html", payload, payload_size, out_archive,
                              out_size)) {
        free(wrapped);
        if (out_archive) *out_archive = NULL;
        if (out_size) *out_size = 0u;
        return QW_HTMLZ_ERR_OUT_OF_MEMORY;
    }
    free(wrapped);
    return QW_HTMLZ_OK;
}

void qw_htmlz_buffer_free(void *buffer)
{
    free(buffer);
}
