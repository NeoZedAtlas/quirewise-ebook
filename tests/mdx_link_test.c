/* MDict @@@LINK redirect resolution and entry:// prefix stripping.
 *
 * The links.mdx fixture has "alias" -> "@@@LINK=alpha" and an alpha record
 * whose link target uses entry://beta. The rendered HTML must show alpha's
 * definition under the alias heading and must not leak the link scheme.
 */

#include <quirewise/mdx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef QUIREWISE_LINKS_MDX
#error "QUIREWISE_LINKS_MDX must be defined"
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

static const char *find_text(const uint8_t *data, size_t size,
                             const char *needle)
{
    size_t needle_size = strlen(needle);
    size_t index;
    if (!needle_size || needle_size > size) return NULL;
    for (index = 0u; index + needle_size <= size; ++index) {
        if (memcmp(data + index, needle, needle_size) == 0)
            return (const char *)data + index;
    }
    return NULL;
}

int main(void)
{
    uint8_t *fixture = NULL;
    size_t fixture_size = 0u;
    QwMdxDocument *document = NULL;
    const uint8_t *html = NULL;
    const uint8_t *text = NULL;
    size_t html_size = 0u;
    size_t text_size = 0u;
    char error[160] = {0};
    const char *alias;
    const char *definition;

    if (!read_file(QUIREWISE_LINKS_MDX, &fixture, &fixture_size))
        return 1;
    if (qw_mdx_parse(fixture, fixture_size, &document, error,
                     sizeof(error)) != QW_MDX_OK) {
        fprintf(stderr, "FAIL: MDX parse: %s\n", error);
        free(fixture);
        return 1;
    }
    free(fixture);
    if (qw_mdx_document_html(document, &html, &html_size) != QW_MDX_OK ||
        !html || !html_size) {
        fputs("FAIL: MDX HTML missing\n", stderr);
        qw_mdx_document_free(document);
        return 1;
    }
    if (qw_mdx_document_text(document, &text, &text_size) != QW_MDX_OK ||
        !text || !text_size) {
        fputs("FAIL: MDX text missing\n", stderr);
        qw_mdx_document_free(document);
        return 1;
    }
    if (find_text(html, html_size, "@@@LINK=") ||
        find_text(html, html_size, "entry://") ||
        find_text(text, text_size, "@@@LINK=") ||
        find_text(text, text_size, "entry://")) {
        fputs("FAIL: MDict link markers leaked into the output\n", stderr);
        qw_mdx_document_free(document);
        return 1;
    }
    alias = find_text(html, html_size, "<h2>alias</h2>");
    definition = alias ? find_text((const uint8_t *)alias,
                                   html_size - (size_t)(alias -
                                       (const char *)html),
                                   "Alpha definition links to") : NULL;
    if (!alias || !definition) {
        fputs("FAIL: alias redirect was not resolved\n", stderr);
        qw_mdx_document_free(document);
        return 1;
    }
    if (find_text(html, html_size, "href=\"beta\"") == NULL) {
        fputs("FAIL: entry:// link target was not rewritten\n", stderr);
        qw_mdx_document_free(document);
        return 1;
    }
    if (!find_text(text, text_size, "Alpha definition links to")) {
        fputs("FAIL: alias redirect missing from text output\n", stderr);
        qw_mdx_document_free(document);
        return 1;
    }
    qw_mdx_document_free(document);
    puts("Quirewise MDict link tests passed");
    return 0;
}
