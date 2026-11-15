/* MDX + MDD: <img> references resolve through the companion resource file. */

#include <quirewise/mdd.h>
#include <quirewise/mdx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_IMAGES_MDX
#error "QUIREWISE_IMAGES_MDX must be defined"
#endif
#ifndef QUIREWISE_TINY_MDD
#error "QUIREWISE_TINY_MDD must be defined"
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
    uint8_t *mdx = NULL;
    size_t mdx_size = 0u;
    uint8_t *mdd = NULL;
    size_t mdd_size = 0u;
    QwMdxDocument *dictionary = NULL;
    QwMddDocument *resources = NULL;
    const uint8_t *html = NULL;
    size_t html_size = 0u;
    char error[128];

    if (!read_file(QUIREWISE_IMAGES_MDX, &mdx, &mdx_size) || !mdx ||
        !read_file(QUIREWISE_TINY_MDD, &mdd, &mdd_size) || !mdd) {
        fputs("FAIL: cannot read the MDict fixtures\n", stderr);
        return 1;
    }
    error[0] = '\0';
    if (qw_mdx_parse(mdx, mdx_size, &dictionary, error, sizeof(error)) !=
            QW_MDX_OK ||
        !dictionary) {
        fprintf(stderr, "FAIL: MDX parse (%s)\n", error);
        free(mdx);
        free(mdd);
        return 1;
    }
    if (qw_mdd_parse(mdd, mdd_size, &resources, error, sizeof(error)) !=
            QW_MDD_OK ||
        !resources) {
        fprintf(stderr, "FAIL: MDD parse (%s)\n", error);
        qw_mdx_document_free(dictionary);
        free(mdx);
        free(mdd);
        return 1;
    }
    free(mdx);
    free(mdd);
    if (qw_mdx_document_html_with_resources(dictionary, resources, &html,
                                            &html_size) != QW_MDX_OK ||
        !html || !html_size ||
        !contains(html, html_size, "data:image/gif;base64,") ||
        !contains(html, html_size, "Image definition")) {
        fputs("FAIL: MDX image reference was not resolved through the MDD\n",
              stderr);
        qw_mdd_document_free(resources);
        qw_mdx_document_free(dictionary);
        return 1;
    }
    qw_mdd_document_free(resources);
    qw_mdx_document_free(dictionary);
    puts("Quirewise MDX/MDD image tests passed");
    return 0;
}
