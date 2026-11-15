/* TXTZ/HTMLZ writer tests: create an archive, then read it back with the
 * package's own extractors. */

#include <quirewise/htmlz.h>
#include <quirewise/text_archive.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static int contains(const uint8_t *data, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;

    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    static const char text[] = "Quirewise TXTZ sample\nsecond line\n";
    static const char html[] =
        "<html><body><h1>Quirewise HTMLZ sample</h1><p>body</p></body></html>";
    uint8_t *archive = NULL;
    size_t archive_size = 0u;

    if (qw_text_archive_create(QW_TEXT_ARCHIVE_TXTZ, text, sizeof(text) - 1u,
                               &archive, &archive_size) != QW_TEXT_ARCHIVE_OK ||
        !archive || !archive_size)
        fail("TXTZ create failed");
    {
        QwTextArchiveDocument document;
        const char *error = NULL;

        memset(&document, 0, sizeof(document));
        if (qw_text_archive_extract_primary(archive, archive_size,
                                            QW_TEXT_ARCHIVE_TXTZ, &document,
                                            &error) != QW_TEXT_ARCHIVE_OK ||
            !document.text_bytes || document.text_size != sizeof(text) - 1u ||
            memcmp(document.text_bytes, text, document.text_size) != 0) {
            fprintf(stderr, "FAIL: TXTZ did not round-trip (%s)\n",
                    error ? error : "");
            exit(1);
        }
        qw_text_archive_document_free(&document);
    }
    qw_text_archive_buffer_free(archive);

    archive = NULL;
    archive_size = 0u;
    if (qw_htmlz_create(html, sizeof(html) - 1u, &archive, &archive_size) !=
            QW_HTMLZ_OK ||
        !archive || !archive_size)
        fail("HTMLZ create failed");
    {
        QwHtmlzDocument document;
        const char *error = NULL;

        memset(&document, 0, sizeof(document));
        if (qw_htmlz_extract_primary_document(archive, archive_size,
                                              &document, &error) !=
                QW_HTMLZ_OK ||
            !document.html_bytes || !document.html_size) {
            fprintf(stderr, "FAIL: HTMLZ did not round-trip (%s)\n",
                    error ? error : "");
            exit(1);
        }
        if (!contains(document.html_bytes, document.html_size,
                      "Quirewise HTMLZ sample")) {
            fputs("FAIL: HTMLZ lost the document text\n", stderr);
            exit(1);
        }
        qw_htmlz_document_free(&document);
    }
    qw_htmlz_buffer_free(archive);

    /* A fragment is wrapped so the reader can select it as the primary
     * document. */
    archive = NULL;
    archive_size = 0u;
    if (qw_htmlz_create("<h1>fragment</h1>", 18u, &archive, &archive_size) !=
            QW_HTMLZ_OK ||
        !archive)
        fail("HTMLZ fragment create failed");
    {
        QwHtmlzDocument document;
        const char *error = NULL;

        memset(&document, 0, sizeof(document));
        if (qw_htmlz_extract_primary_document(archive, archive_size,
                                              &document, &error) !=
            QW_HTMLZ_OK)
            fail("HTMLZ fragment was not readable");
        if (!contains(document.html_bytes, document.html_size,
                      "<!doctype html>"))
            fail("HTMLZ fragment was not wrapped");
        qw_htmlz_document_free(&document);
    }
    qw_htmlz_buffer_free(archive);

    if (qw_text_archive_create(QW_TEXT_ARCHIVE_TXTZ, NULL, 0u, &archive,
                               &archive_size) !=
        QW_TEXT_ARCHIVE_ERR_INVALID_ARGUMENT)
        fail("NULL text was accepted");
    if (qw_htmlz_create(NULL, 0u, &archive, &archive_size) !=
        QW_HTMLZ_ERR_INVALID_ARGUMENT)
        fail("NULL HTML was accepted");
    qw_text_archive_buffer_free(NULL);
    qw_htmlz_buffer_free(NULL);
    puts("Quirewise archive writer tests passed");
    return 0;
}
