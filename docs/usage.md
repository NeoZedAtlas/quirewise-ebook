# quirewise-ebook usage guide

This guide covers the task-level workflows of `Quirewise::Ebook`. The public
API is declared in `quirewise/epub.h`, `quirewise/mobi.h`, `quirewise/fb2.h`,
`quirewise/pml.h`, `quirewise/htmlz.h` and `quirewise/text_archive.h`.

## 1. Build and integrate

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/quirewise/install
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/quirewise/install
```

Consumer CMake:

```cmake
find_package(QuirewiseEbook CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Ebook)
```

The package config calls `find_dependency` for `QuirewiseRuntime`,
`QuirewiseArchive`, `QuirewiseData` and `QuirewiseText`. Install those packages
into the same prefix before configuring a consumer, or add every prefix to
`CMAKE_PREFIX_PATH`.

## 2. Core model and ownership

All readers take a complete file in memory: `(const void *data, size_t size)`.
The input buffer is borrowed for the call and is never retained.

There are three ownership shapes:

| Reader | Result | Release |
|---|---|---|
| EPUB | opaque `QwEpubDocument *` with borrowed views | `qw_epub_document_free` |
| MOBI | opaque `QwMobiDocument *` with an owned text buffer accessor | `qw_mobi_document_free` |
| FB2 | opaque `QwFB2Document *` with an owned HTML accessor | `qw_fb2_document_free` |
| PML | caller-provided `QwPmlDocument` struct | `qw_pml_document_free` |
| HTMLZ | caller-provided `QwHtmlzDocument` struct | `qw_htmlz_document_free` |
| TXTZ / PMLZ | caller-provided `QwTextArchiveDocument` struct | `qw_text_archive_document_free` |

Rules that hold for every reader:

- EPUB, MOBI and FB2 results are UTF-8. Always use the returned size; do not
  assume a terminator, except that PML and MOBI buffers happen to be
  NUL-terminated. PML and HTMLZ/TXTZ text keeps the payload's original
  byte encoding.
- Views returned by `qw_epub_section_at`, `qw_epub_paragraph_at` and
  `qw_epub_run_at` point into the document. They stay valid until
  `qw_epub_document_free` is called and must not be freed individually.
- The `*_status_string` functions return static strings; do not free them.
- All parse entry points set the output pointer or struct to a safe empty value
  before doing work, including on failure.

## 3. Recipe: extract EPUB chapter text

The example assumes `bytes`/`size` hold the complete EPUB file, loaded with your
own I/O.

```c
QwEpubDocument *book = NULL;
size_t chapter, paragraph, run;

if (qw_epub_parse(bytes, size, &book) != QW_EPUB_OK) {
    fputs("EPUB parse failed\n", stderr);
    return 1;
}
for (chapter = 0; chapter < qw_epub_section_count(book); ++chapter) {
    QwEpubSectionView section;
    if (qw_epub_section_at(book, chapter, &section) != QW_EPUB_OK) continue;
    for (paragraph = 0; paragraph < section.paragraph_count; ++paragraph) {
        QwEpubParagraphView view;
        if (qw_epub_paragraph_at(book, chapter, paragraph, &view) != QW_EPUB_OK)
            continue;
        if (view.heading_level)
            printf("\n[h%u] ", view.heading_level);
        for (run = 0; run < view.run_count; ++run) {
            QwEpubRunView text;
            if (qw_epub_run_at(book, chapter, paragraph, run, &text) !=
                    QW_EPUB_OK || !text.utf8_text)
                continue;
            if (text.style_flags & QW_EPUB_STYLE_CODE) fputc('`', stdout);
            fwrite(text.utf8_text, 1u, text.text_size, stdout);
            if (text.style_flags & QW_EPUB_STYLE_CODE) fputc('`', stdout);
        }
        fputc('\n', stdout);
    }
}
qw_epub_document_free(book);
```

- `section.title` is not populated by the current EPUB reader; use
  `heading_level` on paragraphs instead. `title` may be `NULL` and
  `title_size` may be zero.
- `page_break_before` is set on the first paragraph of every chapter after the
  first readable chapter.
- `style_flags` is a bitmask of `QW_EPUB_STYLE_BOLD`, `_ITALIC`, `_CODE`,
  `_HEADING` and `_QUOTE`.
- `link_target` is kept only for external `http://`, `https://`, `mailto:` and
  `tel:` links. Internal anchors are dropped, so check for `NULL`.
- Adjacent runs with the same style and link are merged and whitespace is
  normalized. Empty paragraphs are not emitted.
- Skipped chapters are not an error by themselves. The parse succeeds when at
  least one readable chapter remains; the caller cannot currently see the
  skipped count through this public API.

## 4. Recipe: extract MOBI/AZW3/AZW text

```c
QwMobiDocument *book = NULL;
QwMobiStatus status;
const uint8_t *text;
size_t text_size = 0u;

if (!qw_mobi_probe(bytes, size)) {
    fputs("not a PalmDB/MOBI container\n", stderr);
    return 1;
}
status = qw_mobi_parse(bytes, size, &book);
if (status == QW_MOBI_ERROR_UNSUPPORTED) {
    fputs("DRM, HUFF/CDIC, or an unsupported text encoding\n", stderr);
    return 1;
}
if (status != QW_MOBI_OK) {
    fprintf(stderr, "MOBI parse failed: %d\n", (int)status);
    return 1;
}
printf("kind: %s\n", qw_mobi_document_kind(book) == QW_MOBI_KIND_AZW3
       ? "AZW3" : "MOBI");
text = qw_mobi_document_text(book, &text_size);
if (!text || !text_size) {
    qw_mobi_document_free(book);
    return 1;
}
fwrite(text, 1u, text_size, stdout);
qw_mobi_document_free(book);
```

`qw_mobi_probe` only validates the PalmDB `BOOKMOBI` signature and the MOBI
header framing; it does not decode the book. `qw_mobi_parse` performs the full
validation. AZW3 is reported when the MOBI version field is 8 or higher.

## 5. Recipe: convert FB2 to HTML

```c
QwFB2Document *fb2 = NULL;
const uint8_t *html;
size_t html_size = 0u;

if (qw_fb2_parse(bytes, size, &fb2) != QW_FB2_OK) {
    fputs("FB2 parse failed (malformed XML or unsupported image)\n", stderr);
    return 1;
}
html = qw_fb2_document_html(fb2, &html_size);
if (!html || !html_size) {
    qw_fb2_document_free(fb2);
    return 1;
}
fwrite(html, 1u, html_size, stdout);
qw_fb2_document_free(fb2);
```

The result is a complete UTF-8 HTML document beginning with `<!doctype html>`.
Embedded images become `<img src="data:image/jpeg;base64,...">` elements.
External image references, duplicate `<binary>` ids, and image types other than
JPEG, PNG or GIF make the whole parse fail with `QW_FB2_ERROR_UNSUPPORTED`.

## 6. Recipe: strip PML commands

```c
QwPmlDocument pml = {0};
QwPmlStatus status = qw_pml_parse(bytes, size, &pml);

if (status != QW_PML_OK) {
    fprintf(stderr, "PML parse failed: %s\n", qw_pml_status_string(status));
    return 1;
}
fwrite(pml.text_bytes, 1u, pml.text_size, stdout);
qw_pml_document_free(&pml);
```

Supported translations are `\p` (blank line), `\n` (newline), `\t` (tab) and
`\\` (literal backslash). Other single-letter PML commands are removed;
`\s`, `\c` and `\x` also consume following digits. Escapes that are not letters
emit the escaped byte. The output keeps the input byte encoding.

## 7. Recipe: pull the primary member from an archive wrapper

HTMLZ:

```c
QwHtmlzDocument htmlz = {0};
const char *error = NULL;
QwHtmlzStatus status = qw_htmlz_extract_primary_document(bytes, size, &htmlz,
                                                         &error);

if (status != QW_HTMLZ_OK) {
    fprintf(stderr, "%s\n", error ? error : qw_htmlz_status_string(status));
    return 1;
}
printf("member %zu, %zu bytes\n", htmlz.archive_entry_index, htmlz.html_size);
fwrite(htmlz.html_bytes, 1u, htmlz.html_size, stdout);
qw_htmlz_document_free(&htmlz);
```

TXTZ and PMLZ share one function:

```c
QwTextArchiveDocument textz = {0};
QwTextArchiveStatus status = qw_text_archive_extract_primary(
    bytes, size, QW_TEXT_ARCHIVE_TXTZ, &textz, NULL);

if (status == QW_TEXT_ARCHIVE_OK)
    fwrite(textz.text_bytes, 1u, textz.text_size, stdout);
qw_text_archive_document_free(&textz);
```

Selection is index-first: `index.html`/`index.htm` for HTMLZ,
`index.txt` for TXTZ and `index.pml` for PMLZ. If no index member qualifies,
the first member with the matching suffix and valid content is used. HTMLZ
also requires an `<html` root or `<!doctype html` marker; TXTZ/PMLZ require
text-like bytes as judged by `Quirewise::Text`. `member_name_bytes` keeps the
ZIP entry-name encoding and is not guaranteed to be UTF-8.

## 8. Status handling

Each reader has its own negative status enum. `0` always means success.

| Reader | Success | Main failures |
|---|---|---|
| EPUB | `QW_EPUB_OK` | `_ERR_INVALID_ARGUMENT`, `_ERR_OUT_OF_MEMORY`, `_ERR_FORMAT`, `_ERR_UNSUPPORTED`, `_ERR_RESOURCE_LIMIT`, `_ERR_DECODE` |
| MOBI | `QW_MOBI_OK` | `_ERROR_INVALID_ARGUMENT`, `_OUT_OF_MEMORY`, `_FORMAT`, `_UNSUPPORTED`, `_DECODE`, `_RESOURCE_LIMIT`, `_UNKNOWN` |
| FB2 | `QW_FB2_OK` | `QW_FB2_ERROR_INVALID_ARGUMENT`, `_OUT_OF_MEMORY`, `_FORMAT`, `_UNSUPPORTED`, `_DECODE`, `_RESOURCE_LIMIT`, `_UNKNOWN` |
| PML | `QW_PML_OK` | `_ERR_INVALID_ARGUMENT`, `_ERR_OUT_OF_MEMORY`, `_ERR_RESOURCE_LIMIT` |
| HTMLZ | `QW_HTMLZ_OK` | `_ERR_INVALID_ARGUMENT`, `_ERR_OUT_OF_MEMORY`, `_ERR_ARCHIVE`, `_ERR_NOT_FOUND`, `_ERR_RESOURCE_LIMIT` |
| TXTZ/PMLZ | `QW_TEXT_ARCHIVE_OK` | `QW_TEXT_ARCHIVE_ERR_INVALID_ARGUMENT`, `_OUT_OF_MEMORY`, `_ARCHIVE`, `_NOT_FOUND`, `_RESOURCE_LIMIT` |

- `_UNSUPPORTED` — the container is recognized but uses a feature this package
  does not implement (DRM, HUFF/CDIC, EPUB structural chapter, FB2 image type).
- `_FORMAT` — the bytes are not the format, the container is corrupt, or a
  required part (container.xml, OPF, spine chapter, FB2 body) is missing.
- `_RESOURCE_LIMIT` — a documented cap was exceeded.
- `_NOT_FOUND` — the archive is valid but contains no acceptable member.
- `_DECODE` — text bytes are invalid for the declared encoding.

Only PML, HTMLZ and TXTZ/PMLZ expose `*_status_string`. For the other readers,
map the numeric value in your own diagnostic layer.

## 9. Memory, lifetime and threading

- Every successful parse returns memory that must be released with the matching
  free function, even when the result is an empty document.
- Free functions accept `NULL`/zeroed structs. PML, HTMLZ and text-archive free
  functions zero the struct after releasing it, so double-freeing an already
  cleared struct is safe.
- Do not free the input buffer while a document still uses borrowed views.
  EPUB views and MOBI text accessors borrow from the document, not from the
  input.
- The library has no global mutable state. Independent parses can run on
  separate threads. A finished document may be read concurrently, but it must
  not be freed while another thread is reading it.

## 10. Limits and format notes

| Area | Limit |
|---|---|
| EPUB XML | 16 MiB per `container.xml`, OPF or chapter |
| EPUB spine | 10,000 items |
| EPUB layout | 1,000,000 paragraphs, 4,000,000 runs, 256 MiB of text |
| EPUB ZIP profile | 1 GiB archive input, 10,000 entries, 128 MiB per member, 512 MiB total |
| MOBI text | 32 MiB, no compression or PalmDOC, CP1252 or UTF-8 |
| FB2 HTML | 64 MiB output |
| PML input | 64 MiB |
| HTMLZ/TXTZ/PMLZ | 256 MiB archive, 10,000 entries, 64 MiB per member, 256 MiB total |

Deliberately unsupported: EPUB writing, MOBI/AZW3 writing, DRM removal, HUFF/
CDIC, MOBI embedded resources and fixed layout, EPUB tables/lists/images,
HTMLZ embedded-resource rewriting, and PDF output. PDF and CLI routing belong
to `Quirewise::Document` and `Quirewise::Pipeline`.

## 11. Related packages

- `Quirewise::Runtime` — shared status and format enums.
- `Quirewise::Archive` — ZIP container access used by EPUB and archive wrappers.
- `Quirewise::Data` — XML parsing used by EPUB and FB2.
- `Quirewise::Text` — plain-text detection used by TXTZ/PMLZ.
- `Quirewise::Charset` — legacy encoding conversion when PML or HTMLZ payloads
  are not UTF-8.
- `Quirewise::Document` — layout/PDF rendering for the documents these readers
  produce.

## 12. License

MIT. See `LICENSE`.
