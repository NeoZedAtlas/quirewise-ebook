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
        record = qw_mdict_record_at(container, index, &record_size);
        if (!record) {
            status = QW_MDICT_ERR_FORMAT;
            break;
        }
        record_size = mdict_record_length(record, record_size);
        if (html_records) {
            status = qw_mdict_buffer_append(&html, record, record_size);
            if (status == QW_MDICT_OK)
                status = qw_mdict_buffer_append_text(&html, "\n");
        } else {
            status = mdict_append_escaped_record(&html, record, record_size);
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
        record = qw_mdict_record_at(container, index, &record_size);
        if (!record) {
            status = QW_MDICT_ERR_FORMAT;
            break;
        }
        record_size = mdict_record_length(record, record_size);
        status = mdict_append_text_record(&text, record, record_size,
                                          html_records);
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
