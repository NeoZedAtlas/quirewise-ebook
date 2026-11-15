# quirewise-ebook

Read-only C11 readers for EPUB/KEPUB, MOBI/AZW3/AZW, FB2, PML, HTMLZ, TXTZ,
PMLZ and MDict dictionaries (MDX with its MDD resource companion).

`Quirewise::Ebook` (CMake package `QuirewiseEbook`) is an independent,
MIT-licensed C11 library. It is not dependency-free: its CMake config requires
the installed `Quirewise::Runtime`, `Quirewise::Archive`, `Quirewise::Charset`,
`Quirewise::Compression`, `Quirewise::Crypto`, `Quirewise::Data` and
`Quirewise::Text` packages.

## What it does

- **EPUB / KEPUB** — opens the OCF ZIP container, follows
  `META-INF/container.xml` to the OPF package, walks the spine and returns
  sections, paragraphs and styled text runs. KEPUB books use the same structure.
  Chapters with content the layout cannot represent (images, tables, lists,
  SVG, math, media, embedded objects) are skipped; the rest of the book is
  still returned.
- **MOBI 7 / AZW3 / AZW** — parses the PalmDB/PalmDOC container and returns
  plain UTF-8 text for uncompressed and PalmDOC-compressed books whose text is
  CP1252 or UTF-8. Markup is flattened to paragraphs.
- **FB2** — parses FictionBook 2 XML and returns an owned UTF-8 HTML document
  with headings, paragraphs, emphasis, quotes, poems, links and inline
  JPEG/PNG/GIF images embedded as base64 data URIs.
- **MDX** — parses an MDict dictionary (engine 2.0, MdxBuilder 3.x), decodes
  the zlib/stored index and record blocks, converts keys and articles to UTF-8
  and exposes both the entry list and one rendered HTML or plain-text document.
  The `Encrypted="2"` block scrambling is decoded without a passcode because
  each block key derives from the block's own Adler-32.
- **MDD** — parses the MDict resource companion (images, styles, audio) and
  exposes its resource-name/payload pairs, including exact-name lookup.
- **PML** — strips the supported basic PML commands and returns text.
- **HTMLZ** — extracts the primary HTML/XHTML member from a ZIP archive.
- **TXTZ / PMLZ** — extracts the primary text member from a ZIP archive.

Every reader is read-only. There is no EPUB/MOBI/AZW3/FB2/MDX writer, no PDF
emission, no rendering, no DRM support, no HUFF/CDIC decompression, and no
MDict engine 1.2 (LZO) or register-code encrypted dictionary support.

## Requirements

- C11 compiler and CMake 3.21 or newer.
- Installed Quirewise Runtime, Archive, Charset, Compression, Crypto, Data and
  Text packages, found through `CMAKE_PREFIX_PATH`.
- CTest for the test suite; no third-party libraries.

## Build, test and install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/quirewise/install
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/quirewise/install
```

Shared builds are available with `-DQUIREWISE_BUILD_SHARED=ON`.

## Use it from CMake

```cmake
find_package(QuirewiseEbook CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Ebook)
```

`QuirewiseEbookConfig.cmake` resolves Runtime, Archive, Charset, Compression,
Crypto, Data and Text as well, so the same prefix must expose all of them.

## Quick start

Dump the text of every supported EPUB chapter, given the file bytes loaded with
your own I/O:

```c
#include <quirewise/epub.h>

#include <stdio.h>

int dump_epub(const void *data, size_t size)
{
    QwEpubDocument *book = NULL;
    size_t chapter, paragraph, run;

    if (qw_epub_parse(data, size, &book) != QW_EPUB_OK) return 0;
    for (chapter = 0; chapter < qw_epub_section_count(book); ++chapter) {
        QwEpubSectionView section;
        if (qw_epub_section_at(book, chapter, &section) != QW_EPUB_OK) continue;
        for (paragraph = 0; paragraph < section.paragraph_count; ++paragraph) {
            QwEpubParagraphView view;
            if (qw_epub_paragraph_at(book, chapter, paragraph, &view) !=
                QW_EPUB_OK) continue;
            for (run = 0; run < view.run_count; ++run) {
                QwEpubRunView text;
                if (qw_epub_run_at(book, chapter, paragraph, run, &text) ==
                        QW_EPUB_OK && text.utf8_text)
                    fwrite(text.utf8_text, 1u, text.text_size, stdout);
            }
            fputc('\n', stdout);
        }
    }
    qw_epub_document_free(book);
    return 1;
}
```

It returns `0` when the input is not a readable EPUB or has no supported
chapter. The input buffer may be freed after `qw_epub_parse` returns; views
borrow memory from the document, so free the document last.

## API overview

| Header | Public entry points |
|---|---|
| `quirewise/epub.h` | `qw_epub_parse`, `qw_epub_document_free`, `qw_epub_section_count`, `qw_epub_section_at`, `qw_epub_paragraph_at`, `qw_epub_run_at` |
| `quirewise/mobi.h` | `qw_mobi_probe`, `qw_mobi_parse`, `qw_mobi_document_kind`, `qw_mobi_document_text`, `qw_mobi_document_free` |
| `quirewise/mdx.h` | `qw_mdx_probe`, `qw_mdx_parse`, `qw_mdx_document_title`, `qw_mdx_document_description`, `qw_mdx_document_encoding`, `qw_mdx_document_entry_count`, `qw_mdx_document_entry_at`, `qw_mdx_document_html`, `qw_mdx_document_text`, `qw_mdx_document_free` |
| `quirewise/mdd.h` | `qw_mdd_probe`, `qw_mdd_parse`, `qw_mdd_document_title`, `qw_mdd_document_resource_count`, `qw_mdd_document_resource_at`, `qw_mdd_document_resource_find`, `qw_mdd_document_free` |
| `quirewise/fb2.h` | `qw_fb2_parse`, `qw_fb2_document_html`, `qw_fb2_document_free` |
| `quirewise/pml.h` | `qw_pml_abi_version`, `qw_pml_parse`, `qw_pml_document_free`, `qw_pml_status_string` |
| `quirewise/htmlz.h` | `qw_htmlz_abi_version`, `qw_htmlz_extract_primary_document`, `qw_htmlz_document_free`, `qw_htmlz_status_string` |
| `quirewise/text_archive.h` | `qw_text_archive_abi_version`, `qw_text_archive_extract_primary`, `qw_text_archive_document_free`, `qw_text_archive_status_string` |

## Behavior and limits

- Input buffers are borrowed for the call. Output is owned by the caller or by
  the returned document; no function retains an input pointer.
- EPUB: 16 MiB per XML document, 10,000 spine items, 1,000,000 paragraphs,
  4,000,000 runs and 256 MiB of layout text. Its ZIP profile allows 1 GiB of
  input, 10,000 entries, 128 MiB per member and 512 MiB total.
- EPUB chapters with unsupported structure are skipped. If every chapter is
  skipped, `qw_epub_parse` returns `QW_EPUB_ERR_UNSUPPORTED`; if none contains
  text, it returns `QW_EPUB_ERR_FORMAT`. Styling-only markup (`class`, `style`,
  `<link>`, `<style>`) does not trigger the skip path.
- MOBI text is capped at 32 MiB. Only compression 1 (none) and 2 (PalmDOC) are
  accepted, encoding must be CP1252 or UTF-8, encryption must be zero and the
  MOBI version must be 8 or lower. HUFF/CDIC and DRM return
  `QW_MOBI_ERROR_UNSUPPORTED`; embedded resources and fixed layouts are ignored
  and only the text stream is returned. Version 8 books report
  `QW_MOBI_KIND_AZW3`.
- FB2 HTML is capped at 64 MiB. Images must be embedded `<binary>` elements
  with a unique `id`, referenced as `#id`, and typed `image/jpeg`, `image/png`
  or `image/gif`; anything else fails the parse.
- MDX/MDD accept engine 2.0 containers only, with stored or zlib blocks, plain
  or `Encrypted="2"` (checksum-derived block keys, no passcode). Engine 1.2
  (LZO) and register-code encryption (`Encrypted="1"`/`"3"`, Salsa-20 blocks)
  return `QW_MDX_ERROR_UNSUPPORTED`/`QW_MDD_ERROR_UNSUPPORTED`. Caps: 1 MiB
  header, 32 MiB key index, 64 MiB per block, 192 MiB record stream, 96 MiB
  rendered HTML or text and 1,048,576 entries. Keys and articles are converted
  to UTF-8 from UTF-8, UTF-16 or a `Quirewise::Charset` code page (GBK, Big5,
  Shift-JIS, ...). MDD payloads stay binary.
- MDict rendering does not embed MDD images or resolve `@@@LINK`/`entry://`
  article links; a dictionary that relies on them renders the surrounding text
  only. The text rendering reduces block markup (`p`, `br`, `div`, headings,
  list and table rows) to line breaks.
- PML accepts non-empty input up to 64 MiB and preserves the input byte
  encoding; it does not transcode to UTF-8.
- HTMLZ and TXTZ/PMLZ accept archives up to 256 MiB with at most 10,000
  entries, 64 MiB per member and 256 MiB total uncompressed data. Member names
  are limited to 4,096 bytes. Index members win first; otherwise the first
  member with a matching suffix and valid content wins.
- Error codes are per-format negative enums (`QwEpubStatus`, `QwMobiStatus`,
  `QwFB2Status`, `QwMdxStatus`, `QwMddStatus`, `QwPmlStatus`, `QwHtmlzStatus`,
  `QwTextArchiveStatus`), not the shared `QwStatus`. PML, HTMLZ and TXTZ/PMLZ
  also provide `*_status_string`.
- There is no global mutable state. Separate documents can be parsed in
  parallel; a finished document is safe for concurrent read-only traversal as
  long as no thread frees it.

## Testing

`ctest` runs `quirewise_ebook_fuzz_smoke` (seeded mutations against EPUB, HTMLZ,
TXTZ, FB2, PML and MOBI), `quirewise_ebook_mobi_palm_doc` (PalmDOC
back-references across records and record padding) and `quirewise_ebook_mdict`
(synthetic MDX/MDD containers, plain and `Encrypted="2"`, plus truncation and
checksum rejection).

## Documentation

See [docs/usage.md](docs/usage.md) for task-oriented recipes, ownership rules,
status tables and per-format limits.

## License

MIT. See [LICENSE](LICENSE).
