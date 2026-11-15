/*
 * Rendering for MDict dictionaries: one HTML document for the layout/PDF
 * route and one plain-text transcript for the text route.
 *
 * Records are stored as HTML fragments in "Html" dictionaries (the common
 * case) and as plain text otherwise. The layout renderer understands block
 * markup only, so image references resolve to nothing; that is why the HTML
 * report notes omitted images rather than failing.
 */

#include "mdict_internal.h"

#include <quirewise/mdd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int qw_mdict_append_escaped(QwMdictBuffer *buffer, const uint8_t *text,
                            size_t size)
{
    size_t index;

    if (!buffer || (!text && size)) return QW_MDICT_ERR_INVALID_ARGUMENT;
    for (index = 0u; index < size; ++index) {
        int status;
        switch (text[index]) {
        case '&': status = qw_mdict_buffer_append_text(buffer, "&amp;"); break;
        case '<': status = qw_mdict_buffer_append_text(buffer, "&lt;"); break;
        case '>': status = qw_mdict_buffer_append_text(buffer, "&gt;"); break;
        case '"': status = qw_mdict_buffer_append_text(buffer, "&quot;"); break;
        default: status = qw_mdict_buffer_append_byte(buffer, text[index]); break;
        }
        if (status != QW_MDICT_OK) return status;
    }
    return QW_MDICT_OK;
}

static int mdict_text_is_html(const char *format)
{
    char first;
    char second;
    char third;
    char fourth;

    if (!format) return 1;
    first = format[0];
    second = format[1];
    third = format[2];
    fourth = format[3];
    if (first >= 'a' && first <= 'z') first = (char)(first - 'a' + 'A');
    if (second >= 'a' && second <= 'z') second = (char)(second - 'a' + 'A');
    if (third >= 'a' && third <= 'z') third = (char)(third - 'a' + 'A');
    if (fourth >= 'a' && fourth <= 'z') fourth = (char)(fourth - 'a' + 'A');
    return first == 'H' && second == 'T' && third == 'M' && fourth == 'L';
}

/* A record slice ends with the filler that pads the next entry's boundary. */
static size_t mdict_record_length(const uint8_t *record, size_t size)
{
    while (size && record[size - 1u] == 0u) --size;
    return size;
}

static int mdict_append_escaped_record(QwMdictBuffer *buffer,
                                       const uint8_t *record, size_t size)
{
    size_t index;
    int status = qw_mdict_buffer_append_text(buffer, "<p>");

    if (status != QW_MDICT_OK) return status;
    for (index = 0u; index < size; ++index) {
        if (record[index] == '\r') continue;
        if (record[index] == '\n') {
            status = qw_mdict_buffer_append_text(buffer, "</p><p>");
            if (status != QW_MDICT_OK) return status;
            continue;
        }
        status = qw_mdict_append_escaped(buffer, record + index, 1u);
        if (status != QW_MDICT_OK) return status;
    }
    return qw_mdict_buffer_append_text(buffer, "</p>\n");
}


static int mdict_ascii_folded_equal(const uint8_t *left, size_t left_size,
                                    const uint8_t *right, size_t right_size)
{
    size_t index;
    if (left_size != right_size) return 0;
    for (index = 0u; index < left_size; ++index) {
        unsigned char a = left[index];
        unsigned char b = right[index];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

/* Resolve "@@@LINK=target" redirect chains (bounded to 8 hops). */
static const uint8_t *mdict_resolve_record(const QwMdictContainer *container,
                                           size_t index, size_t *out_size)
{
    const uint8_t *record;
    size_t depth;

    if (!container || !out_size) return NULL;
    record = qw_mdict_record_at(container, index, out_size);
    for (depth = 0u; record && depth < 8u; ++depth) {
        size_t size = mdict_record_length(record, *out_size);
        size_t target_size;
        const uint8_t *target;
        size_t next;
        if (size < 8u || memcmp(record, "@@@LINK=", 8u) != 0) break;
        target = record + 8u;
        target_size = size - 8u;
        while (target_size &&
               (target[target_size - 1u] == '\r' ||
                target[target_size - 1u] == '\n'))
            --target_size;
        for (next = 0u; next < container->table.count; ++next) {
            if (mdict_ascii_folded_equal(
                    container->table.key_text.data +
                        container->table.key_offsets[next],
                    container->table.key_lengths[next], target,
                    target_size))
                break;
        }
        if (next >= container->table.count) break;
        record = qw_mdict_record_at(container, next, out_size);
    }
    return record;
}

/* entry://word -> word: the layout/text routes cannot follow dictionary
 * scheme links, so the prefix is stripped and the target stays visible. */
static uint8_t *mdict_strip_entry_scheme(const uint8_t *record, size_t size,
                                         size_t *out_size)
{
    uint8_t *stripped;
    size_t index = 0u;
    size_t written = 0u;

    if (!record || !out_size) return NULL;
    stripped = (uint8_t *)malloc(size + 1u);
    if (!stripped) return NULL;
    while (index < size) {
        if (index + 8u <= size &&
            mdict_ascii_folded_equal(record + index, 8u,
                                     (const uint8_t *)"entry://", 8u)) {
            index += 8u;
            continue;
        }
        stripped[written++] = record[index++];
    }
    stripped[written] = 0u;
    *out_size = written;
    return stripped;
}

static int mdict_ascii_equal_ci(const uint8_t *text, size_t size,
                                const char *word)
{
    size_t i;
    size_t word_size = strlen(word);
    if (size != word_size) return 0;
    for (i = 0u; i < size; ++i) {
        unsigned char a = text[i];
        unsigned char b = (unsigned char)word[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
        if (a != b) return 0;
    }
    return 1;
}

static const char *mdict_image_mime_from_name(const char *name, size_t size)
{
    size_t i;
    const char *dot = NULL;
    for (i = 0u; i < size; ++i) {
        if (name[i] == '.') dot = name + i;
    }
    if (!dot) return NULL;
    if (mdict_ascii_equal_ci((const uint8_t *)dot, size - (size_t)(dot - name),
                             ".png"))
        return "image/png";
    if (mdict_ascii_equal_ci((const uint8_t *)dot, size - (size_t)(dot - name),
                             ".jpg") ||
        mdict_ascii_equal_ci((const uint8_t *)dot, size - (size_t)(dot - name),
                             ".jpeg"))
        return "image/jpeg";
    if (mdict_ascii_equal_ci((const uint8_t *)dot, size - (size_t)(dot - name),
                             ".gif"))
        return "image/gif";
    if (mdict_ascii_equal_ci((const uint8_t *)dot, size - (size_t)(dot - name),
                             ".webp"))
        return "image/webp";
    if (mdict_ascii_equal_ci((const uint8_t *)dot, size - (size_t)(dot - name),
                             ".bmp"))
        return "image/bmp";
    return NULL;
}

static int mdict_find_img_src(const uint8_t *tag, size_t size,
                              size_t *out_start, size_t *out_end)
{
    size_t i = 0u;
    while (i + 4u <= size) {
        size_t j;
        if (i != 0u) {
            unsigned char prev = tag[i - 1u];
            if (!(prev == ' ' || prev == '\t' || prev == '\r' ||
                  prev == '\n' || prev == '/'))
                { ++i; continue; }
        }
        if (!((tag[i] == 's' || tag[i] == 'S') &&
              (tag[i + 1u] == 'r' || tag[i + 1u] == 'R') &&
              (tag[i + 2u] == 'c' || tag[i + 2u] == 'C'))) {
            ++i;
            continue;
        }
        j = i + 3u;
        while (j < size && (tag[j] == ' ' || tag[j] == '\t' ||
                            tag[j] == '\r' || tag[j] == '\n'))
            ++j;
        if (j >= size || tag[j] != '=') { ++i; continue; }
        ++j;
        while (j < size && (tag[j] == ' ' || tag[j] == '\t' ||
                            tag[j] == '\r' || tag[j] == '\n'))
            ++j;
        if (j >= size || (tag[j] != '"' && tag[j] != '\'')) { ++i; continue; }
        {
            unsigned char quote = tag[j];
            size_t start = ++j;
            while (j < size && tag[j] != quote) ++j;
            if (j >= size) return 0;
            *out_start = start;
            *out_end = j;
            return 1;
        }
    }
    return 0;
}

static int mdict_append_base64(QwMdictBuffer *html, const uint8_t *data,
                               size_t size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0u; i < size; i += 3u) {
        uint32_t value = (uint32_t)data[i] << 16;
        size_t remain = size - i;
        char encoded[5];
        if (remain > 1u) value |= (uint32_t)data[i + 1u] << 8;
        if (remain > 2u) value |= data[i + 2u];
        encoded[0] = alphabet[(value >> 18) & 63u];
        encoded[1] = alphabet[(value >> 12) & 63u];
        encoded[2] = remain > 1u ? alphabet[(value >> 6) & 63u] : '=';
        encoded[3] = remain > 2u ? alphabet[value & 63u] : '=';
        encoded[4] = '\0';
        if (qw_mdict_buffer_append_text(html, encoded) != QW_MDICT_OK)
            return QW_MDICT_ERR_OUT_OF_MEMORY;
    }
    return QW_MDICT_OK;
}

static int mdict_lookup_image(const QwMddDocument *mdd, const char *value,
                              size_t value_size, const uint8_t **out_data,
                              size_t *out_size)
{
    const char *base = value;
    size_t base_size = value_size;
    size_t i;
    char candidate[512];
    for (i = 0u; i < value_size; ++i) {
        if (value[i] == '\\' || value[i] == '/') {
            base = value + i + 1u;
            base_size = value_size - i - 1u;
        }
    }
    if (base_size == 0u || base_size >= sizeof(candidate) - 1u) return 0;
    if (qw_mdd_document_resource_find(mdd, base, base_size, out_data,
                                      out_size) == QW_MDD_OK)
        return 1;
    candidate[0] = '\\';
    memcpy(candidate + 1u, base, base_size);
    if (qw_mdd_document_resource_find(mdd, candidate, base_size + 1u,
                                      out_data, out_size) == QW_MDD_OK)
        return 1;
    candidate[0] = '/';
    return qw_mdd_document_resource_find(mdd, candidate, base_size + 1u,
                                         out_data, out_size) == QW_MDD_OK;
}

static int mdict_append_record_with_images(const QwMddDocument *mdd,
                                           const uint8_t *record, size_t size,
                                           QwMdictBuffer *html,
                                           size_t *embedded_bytes)
{
    size_t i = 0u;
    while (i < size) {
        size_t chunk_start = i;
        if (record[i] == '<') {
            size_t tag_end = i + 1u;
            while (tag_end < size && record[tag_end] != '>') ++tag_end;
            if (tag_end >= size) break;
            if (tag_end > i + 4u &&
                ((record[i + 1u] == 'i' || record[i + 1u] == 'I') &&
                 (record[i + 2u] == 'm' || record[i + 2u] == 'M') &&
                 (record[i + 3u] == 'g' || record[i + 3u] == 'G')) &&
                (record[i + 4u] == ' ' || record[i + 4u] == '\t' ||
                 record[i + 4u] == '\r' || record[i + 4u] == '\n' ||
                 record[i + 4u] == '/')) {
                size_t value_start = 0u;
                size_t value_end = 0u;
                if (mdict_find_img_src(record + i + 1u,
                                       tag_end - (i + 1u), &value_start,
                                       &value_end)) {
                    const uint8_t *data = NULL;
                    size_t data_size = 0u;
                    const char *mime;
                    value_start += i + 1u;
                    value_end += i + 1u;
                    mime = mdict_image_mime_from_name(
                        (const char *)record + value_start,
                        value_end - value_start);
                    if (mime && mdd &&
                        mdict_lookup_image(mdd,
                                           (const char *)record + value_start,
                                           value_end - value_start, &data,
                                           &data_size) &&
                        data_size > 0u && data_size <= 8u * 1024u * 1024u &&
                        *embedded_bytes + data_size <= 32u * 1024u * 1024u) {
                        int status = qw_mdict_buffer_append(
                            html, record + chunk_start,
                            value_start - chunk_start);
                        if (status == QW_MDICT_OK)
                            status = qw_mdict_buffer_append_text(
                                html, "data:");
                        if (status == QW_MDICT_OK)
                            status = qw_mdict_buffer_append_text(html, mime);
                        if (status == QW_MDICT_OK)
                            status = qw_mdict_buffer_append_text(
                                html, ";base64,");
                        if (status == QW_MDICT_OK)
                            status = mdict_append_base64(html, data,
                                                         data_size);
                        if (status == QW_MDICT_OK)
                            status = qw_mdict_buffer_append(
                                html, record + value_end,
                                tag_end + 1u - value_end);
                        if (status != QW_MDICT_OK) return status;
                        *embedded_bytes += data_size;
                        i = tag_end + 1u;
                        continue;
                    }
                }
            }
            if (qw_mdict_buffer_append(html, record + i,
                                       tag_end + 1u - i) != QW_MDICT_OK)
                return QW_MDICT_ERR_OUT_OF_MEMORY;
            i = tag_end + 1u;
            continue;
        }
        while (i < size && record[i] != '<') ++i;
        if (qw_mdict_buffer_append(html, record + chunk_start,
                                   i - chunk_start) != QW_MDICT_OK)
            return QW_MDICT_ERR_OUT_OF_MEMORY;
    }
    return QW_MDICT_OK;
}

int qw_mdict_build_html_with_resources(const QwMdictContainer *container,
                                       const void *mdd_document,
                                       uint8_t **out_html, size_t *out_size,
                                       char *error, size_t error_capacity)
{
    const QwMddDocument *mdd = (const QwMddDocument *)mdd_document;
    QwMdictBuffer html;
    int html_records;
    size_t index;
    size_t embedded_bytes = 0u;
    int status = QW_MDICT_OK;

    if (out_html) *out_html = NULL;
    if (out_size) *out_size = 0u;
    if (!container || !out_html || !out_size)
        return QW_MDICT_ERR_INVALID_ARGUMENT;
    memset(&html, 0, sizeof(html));
    html.limit = QW_MDICT_MAX_TEXT_BYTES;
    html_records = mdict_text_is_html(container->meta.format);

    status = qw_mdict_buffer_append_text(&html, "<html><body>\n");
    if (status == QW_MDICT_OK && container->meta.title &&
        container->meta.title[0]) {
        status = qw_mdict_buffer_append_text(&html, "<h1>");
        if (status == QW_MDICT_OK)
            status = qw_mdict_append_escaped(
                &html, (const uint8_t *)container->meta.title,
                strlen(container->meta.title));
        if (status == QW_MDICT_OK)
            status = qw_mdict_buffer_append_text(&html, "</h1>\n");
    }
    if (status == QW_MDICT_OK && container->meta.description &&
        container->meta.description[0]) {
        status = qw_mdict_buffer_append_text(&html, "<div>");
        if (status == QW_MDICT_OK && html_records) {
            status = qw_mdict_buffer_append(
                &html, container->meta.description,
                strlen(container->meta.description));
        } else if (status == QW_MDICT_OK) {
            status = qw_mdict_append_escaped(
                &html, (const uint8_t *)container->meta.description,
                strlen(container->meta.description));
        }
        if (status == QW_MDICT_OK)
            status = qw_mdict_buffer_append_text(&html, "</div>\n");
    }

    for (index = 0u; status == QW_MDICT_OK && index < container->table.count;
         ++index) {
        const uint8_t *record;
        size_t record_size = 0u;
        const uint8_t *key = container->table.key_text.data +
                             container->table.key_offsets[index];

        status = qw_mdict_buffer_append_text(&html, "<h2>");
        if (status != QW_MDICT_OK) break;
        status = qw_mdict_append_escaped(
            &html, key, container->table.key_lengths[index]);
        if (status != QW_MDICT_OK) break;
        status = qw_mdict_buffer_append_text(&html, "</h2>\n");
        if (status != QW_MDICT_OK) break;
        record = mdict_resolve_record(container, index, &record_size);
        if (!record) {
            status = QW_MDICT_ERR_FORMAT;
            break;
        }
        record_size = mdict_record_length(record, record_size);
        {
            uint8_t *stripped = mdict_strip_entry_scheme(record, record_size,
                                                         &record_size);
            if (stripped) record = stripped;
            if (html_records) {
                if (mdd)
                    status = mdict_append_record_with_images(
                        mdd, record, record_size, &html, &embedded_bytes);
                else
                    status = qw_mdict_buffer_append(&html, record,
                                                   record_size);
                if (status == QW_MDICT_OK)
                    status = qw_mdict_buffer_append_text(&html, "\n");
            } else {
                status = mdict_append_escaped_record(&html, record,
                                                     record_size);
            }
            free(stripped);
        }
    }
    if (status != QW_MDICT_OK) {
        qw_mdict_buffer_free(&html);
        if (error && error_capacity)
            snprintf(error, error_capacity, "%s",
                     status == QW_MDICT_ERR_RESOURCE_LIMIT
                         ? "MDict document exceeds the rendered text limit"
                         : "MDict document could not be rendered");
        return status;
    }
    if (qw_mdict_buffer_append_text(&html, "</body></html>\n") != QW_MDICT_OK ||
        qw_mdict_buffer_append_byte(&html, 0u) != QW_MDICT_OK) {
        qw_mdict_buffer_free(&html);
        return QW_MDICT_ERR_OUT_OF_MEMORY;
    }
    *out_html = html.data;
    *out_size = html.size - 1u;
    return QW_MDICT_OK;
}

int qw_mdict_build_html(const QwMdictContainer *container, uint8_t **out_html,
                        size_t *out_size, char *error, size_t error_capacity)
{
    QwMdictBuffer html;
    int html_records;
    size_t index;
    int status = QW_MDICT_OK;

    if (out_html) *out_html = NULL;
    if (out_size) *out_size = 0u;
    if (!container || !out_html || !out_size)
        return QW_MDICT_ERR_INVALID_ARGUMENT;
    memset(&html, 0, sizeof(html));
    html.limit = QW_MDICT_MAX_TEXT_BYTES;
    html_records = mdict_text_is_html(container->meta.format);

    status = qw_mdict_buffer_append_text(&html, "<html><body>\n");
    if (status == QW_MDICT_OK && container->meta.title &&
        container->meta.title[0]) {
        status = qw_mdict_buffer_append_text(&html, "<h1>");
        if (status == QW_MDICT_OK)
            status = qw_mdict_append_escaped(
                &html, (const uint8_t *)container->meta.title,
                strlen(container->meta.title));
        if (status == QW_MDICT_OK)
            status = qw_mdict_buffer_append_text(&html, "</h1>\n");
    }
    if (status == QW_MDICT_OK && container->meta.description &&
        container->meta.description[0]) {
        status = qw_mdict_buffer_append_text(&html, "<div>");
        if (status == QW_MDICT_OK && html_records) {
            status = qw_mdict_buffer_append(
                &html, container->meta.description,
                strlen(container->meta.description));
        } else if (status == QW_MDICT_OK) {
            status = qw_mdict_append_escaped(
                &html, (const uint8_t *)container->meta.description,
                strlen(container->meta.description));
        }
        if (status == QW_MDICT_OK)
            status = qw_mdict_buffer_append_text(&html, "</div>\n");
    }

    for (index = 0u; status == QW_MDICT_OK && index < container->table.count;
         ++index) {
        const uint8_t *record;
        size_t record_size = 0u;
        const uint8_t *key = container->table.key_text.data +
                             container->table.key_offsets[index];

        status = qw_mdict_buffer_append_text(&html, "<h2>");
        if (status != QW_MDICT_OK) break;
        status = qw_mdict_append_escaped(
            &html, key, container->table.key_lengths[index]);
        if (status != QW_MDICT_OK) break;
        status = qw_mdict_buffer_append_text(&html, "</h2>\n");
        if (status != QW_MDICT_OK) break;
        record = mdict_resolve_record(container, index, &record_size);
        if (!record) {
            status = QW_MDICT_ERR_FORMAT;
            break;
        }
        record_size = mdict_record_length(record, record_size);
        {
            uint8_t *stripped = mdict_strip_entry_scheme(record, record_size,
                                                         &record_size);
            if (stripped) record = stripped;
            if (html_records) {
                status = qw_mdict_buffer_append(&html, record, record_size);
                if (status == QW_MDICT_OK)
                    status = qw_mdict_buffer_append_text(&html, "\n");
            } else {
                status = mdict_append_escaped_record(&html, record,
                                                     record_size);
            }
            free(stripped);
        }
    }
    if (status != QW_MDICT_OK) {
        qw_mdict_buffer_free(&html);
        if (error && error_capacity)
            snprintf(error, error_capacity, "%s",
                     status == QW_MDICT_ERR_RESOURCE_LIMIT
                         ? "MDict document exceeds the rendered text limit"
                         : "MDict document could not be rendered");
        return status;
    }
    if (qw_mdict_buffer_append_text(&html, "</body></html>\n") != QW_MDICT_OK ||
        qw_mdict_buffer_append_byte(&html, 0u) != QW_MDICT_OK) {
        qw_mdict_buffer_free(&html);
        return QW_MDICT_ERR_OUT_OF_MEMORY;
    }
    *out_html = html.data;
    *out_size = html.size - 1u;
    return QW_MDICT_OK;
}

/* Block-level tag names that end the current line in a text transcript. */
static int mdict_tag_is_block(const uint8_t *name, size_t length)
{
    static const char *const block_tags[] = {
        "p", "div", "br", "li", "ul", "ol", "dl", "dt", "dd", "tr", "td",
        "th", "table", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote",
        "section", "article", "hr", "pre", "caption", "figure"
    };
    size_t index;

    for (index = 0u; index < sizeof(block_tags) / sizeof(block_tags[0]);
         ++index) {
        size_t candidate = strlen(block_tags[index]);
        size_t offset = 0u;
        if (candidate != length) continue;
        while (offset < length) {
            char left = (char)name[offset];
            char right = block_tags[index][offset];
            if (left >= 'A' && left <= 'Z') left = (char)(left - 'A' + 'a');
            if (left != right) break;
            ++offset;
        }
        if (offset == length) return 1;
    }
    return 0;
}

/* Append one record as text: block tags become line breaks and inline tags
 * are dropped. Records of a plain-text dictionary pass through unchanged. */
static int mdict_append_text_record(QwMdictBuffer *buffer,
                                    const uint8_t *record, size_t size,
                                    int html_records)
{
    size_t index = 0u;

    if (!html_records)
        return qw_mdict_append_unescaped(buffer, record, size);
    while (index < size) {
        int status;
        if (record[index] == '<') {
            size_t end = index + 1u;
            size_t name_start;
            size_t name_length;
            while (end < size && record[end] != '>') ++end;
            if (end >= size) {
                status = qw_mdict_buffer_append_byte(buffer, '<');
                if (status != QW_MDICT_OK) return status;
                ++index;
                continue;
            }
            name_start = index + 1u;
            if (name_start < end && record[name_start] == '/') ++name_start;
            name_length = 0u;
            while (name_start + name_length < end) {
                uint8_t value = record[name_start + name_length];
                if (!((value >= 'a' && value <= 'z') ||
                      (value >= 'A' && value <= 'Z') ||
                      (value >= '0' && value <= '9') || value == '_'))
                    break;
                ++name_length;
            }
            if (mdict_tag_is_block(record + name_start, name_length) &&
                buffer->size && buffer->data[buffer->size - 1u] != '\n') {
                status = qw_mdict_buffer_append_byte(buffer, '\n');
                if (status != QW_MDICT_OK) return status;
            }
            index = end + 1u;
            continue;
        }
        if (record[index] == '&') {
            size_t end = index + 1u;
            while (end < size && end - index <= 10u && record[end] != ';')
                ++end;
            if (end < size && record[end] == ';') {
                int status = qw_mdict_append_unescaped(
                    buffer, record + index, end - index + 1u);
                if (status != QW_MDICT_OK) return status;
                index = end + 1u;
                continue;
            }
        }
        if (record[index] == '\r') {
            ++index;
            continue;
        }
        if (qw_mdict_buffer_append_byte(buffer, record[index]) != QW_MDICT_OK)
            return QW_MDICT_ERR_OUT_OF_MEMORY;
        ++index;
    }
    return QW_MDICT_OK;
}

int qw_mdict_build_text(const QwMdictContainer *container, uint8_t **out_text,
                        size_t *out_size, char *error, size_t error_capacity)
{
    QwMdictBuffer text;
    int html_records;
    size_t index;
    int status = QW_MDICT_OK;

    if (out_text) *out_text = NULL;
    if (out_size) *out_size = 0u;
    if (!container || !out_text || !out_size)
        return QW_MDICT_ERR_INVALID_ARGUMENT;
    memset(&text, 0, sizeof(text));
    text.limit = QW_MDICT_MAX_TEXT_BYTES;
    html_records = mdict_text_is_html(container->meta.format);

    for (index = 0u; status == QW_MDICT_OK && index < container->table.count;
         ++index) {
        const uint8_t *record;
        size_t record_size = 0u;
        const uint8_t *key = container->table.key_text.data +
                             container->table.key_offsets[index];

        status = qw_mdict_buffer_append(
            &text, key, container->table.key_lengths[index]);
        if (status != QW_MDICT_OK) break;
        if (container->table.key_lengths[index]) {
            status = qw_mdict_buffer_append_byte(&text, '\n');
            if (status != QW_MDICT_OK) break;
        }
        record = mdict_resolve_record(container, index, &record_size);
        if (!record) {
            status = QW_MDICT_ERR_FORMAT;
            break;
        }
        record_size = mdict_record_length(record, record_size);
        {
            uint8_t *stripped = mdict_strip_entry_scheme(record, record_size,
                                                         &record_size);
            if (stripped) record = stripped;
            status = mdict_append_text_record(&text, record, record_size,
                                              html_records);
            free(stripped);
        }
        if (status != QW_MDICT_OK) break;
        if (!text.size || text.data[text.size - 1u] != '\n') {
            status = qw_mdict_buffer_append_byte(&text, '\n');
            if (status != QW_MDICT_OK) break;
        }
        status = qw_mdict_buffer_append_byte(&text, '\n');
    }
    if (status != QW_MDICT_OK) {
        qw_mdict_buffer_free(&text);
        if (error && error_capacity)
            snprintf(error, error_capacity, "%s",
                     status == QW_MDICT_ERR_RESOURCE_LIMIT
                         ? "MDict text exceeds the rendered text limit"
                         : "MDict text could not be rendered");
        return status;
    }
    if (qw_mdict_buffer_append_byte(&text, 0u) != QW_MDICT_OK) {
        qw_mdict_buffer_free(&text);
        return QW_MDICT_ERR_OUT_OF_MEMORY;
    }
    *out_text = text.data;
    *out_size = text.size - 1u;
    return QW_MDICT_OK;
}
