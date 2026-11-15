/* EPUB image embedding: <img src> becomes a data: URI run. */

#include <quirewise/epub.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_EPUB_IMAGE_FIXTURE
#error "QUIREWISE_EPUB_IMAGE_FIXTURE must be defined"
#endif

static int read_file(const char *path, uint8_t **out, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
    data = (uint8_t *)malloc((size_t)size);
    if (!data) { fclose(file); return 0; }
    if (fread(data, 1u, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out = data;
    *out_size = (size_t)size;
    return 1;
}

int main(void)
{
    uint8_t *epub = NULL;
    size_t epub_size = 0u;
    QwEpubDocument *document = NULL;
    QwEpubSectionView section;
    size_t found = 0u;
    size_t paragraph_index;

    if (!read_file(QUIREWISE_EPUB_IMAGE_FIXTURE, &epub, &epub_size))
        return 1;
    if (qw_epub_parse(epub, epub_size, &document) != QW_EPUB_OK ||
        !document) {
        fputs("FAIL: EPUB parse failed\n", stderr);
        free(epub);
        return 1;
    }
    free(epub);
    memset(&section, 0, sizeof(section));
    if (qw_epub_section_at(document, 0u, &section) != QW_EPUB_OK) {
        fputs("FAIL: EPUB section missing\n", stderr);
        qw_epub_document_free(document);
        return 1;
    }
    for (paragraph_index = 0u; paragraph_index < section.paragraph_count;
         ++paragraph_index) {
        QwEpubParagraphView paragraph;
        size_t run_index;
        memset(&paragraph, 0, sizeof(paragraph));
        if (qw_epub_paragraph_at(document, 0u, paragraph_index, &paragraph) !=
            QW_EPUB_OK)
            continue;
        for (run_index = 0u; run_index < paragraph.run_count; ++run_index) {
            QwEpubRunView run;
            memset(&run, 0, sizeof(run));
            if (qw_epub_run_at(document, 0u, paragraph_index, run_index,
                               &run) != QW_EPUB_OK)
                continue;
            if ((run.style_flags & QW_EPUB_STYLE_IMAGE) && run.utf8_text &&
                run.text_size >= strlen("data:image/png;base64,") &&
                memcmp(run.utf8_text, "data:image/png;base64,",
                       strlen("data:image/png;base64,")) == 0)
                ++found;
        }
    }
    qw_epub_document_free(document);
    if (!found) {
        fputs("FAIL: no image data URI run\n", stderr);
        return 1;
    }
    puts("Quirewise EPUB image tests passed");
    return 0;
}
