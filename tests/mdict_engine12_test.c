/* MDict engine 1.2 dictionaries use LZO-compressed blocks. */

#include <quirewise/mdx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_ENGINE12_MDX
#error "QUIREWISE_ENGINE12_MDX must be defined"
#endif

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    long size;
    uint8_t *data;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return NULL; }
    size = ftell(file);
    if (size <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    data = (uint8_t *)malloc((size_t)size);
    if (!data) { fclose(file); return NULL; }
    if (fread(data, 1u, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)size;
    return data;
}

static int contains(const char *text, size_t size, const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;
    if (!needle_size || needle_size > size) return 0;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(text + index, needle, needle_size) == 0) return 1;
    }
    return 0;
}

int main(void)
{
    uint8_t *data;
    size_t size = 0u;
    QwMdxDocument *document = NULL;
    QwMdxStatus status;
    char error[256];
    size_t index;
    static const char *keys[] = {"alpha", "beta", "gamma"};

    data = read_file(QUIREWISE_ENGINE12_MDX, &size);
    if (!data || !size) {
        fputs("FAIL: cannot read the engine 1.2 MDX fixture\n", stderr);
        return 1;
    }
    memset(error, 0, sizeof(error));
    status = qw_mdx_parse(data, size, &document, error, sizeof(error));
    if (status != QW_MDX_OK || !document) {
        fprintf(stderr, "FAIL: engine 1.2 MDX parse (%s)\n", error);
        free(data);
        return 1;
    }
    if (qw_mdx_document_entry_count(document) != 3u) {
        fprintf(stderr, "FAIL: engine 1.2 entry count %zu\n",
                qw_mdx_document_entry_count(document));
        qw_mdx_document_free(document);
        free(data);
        return 1;
    }
    for (index = 0u; index < 3u; ++index) {
        const char *key = NULL;
        const char *record = NULL;
        size_t key_size = 0u;
        size_t record_size = 0u;
        if (qw_mdx_document_entry_at(document, index, &key, &key_size, &record,
                                     &record_size) != QW_MDX_OK ||
            !key || key_size != strlen(keys[index]) ||
            memcmp(key, keys[index], key_size) != 0 || !record ||
            !contains(record, record_size, "Quirewise sample definition")) {
            fprintf(stderr, "FAIL: engine 1.2 entry %zu\n", index);
            qw_mdx_document_free(document);
            free(data);
            return 1;
        }
    }
    qw_mdx_document_free(document);
    free(data);
    puts("Quirewise MDict engine 1.2 LZO tests passed");
    return 0;
}
