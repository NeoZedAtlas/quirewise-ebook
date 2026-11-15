#include "../../core/format_registry.h"
#include "mdict_internal.h"

#include "quirewise/mdd.h"

static int mdd_probe_descriptor(const void *data, size_t size)
{
    return qw_mdd_probe(data, size);
}

/* An MDD is a resource container: it exposes no document, layout or audio
 * adapter of its own, so the registry only advertises detection. */
const QwFormatDescriptor qw_format_mdd = {
    .id = QW_FORMAT_MDD,
    .name = "MDict resource file",
    .capabilities = (QW_FORMAT_CAP_CONTAINER),
    .probe = mdd_probe_descriptor,
    .open_memory = NULL,
    .open_layout = NULL,
    .open_audio = NULL,
    .max_input_bytes = QW_FORMAT_INPUT_LIMIT_DEFAULT,
    .resource_profile = QW_FORMAT_INPUT_RESOURCE_PROFILE_DEFAULT,
    .convert_to_pdf = NULL,
    .open_spreadsheet = NULL,
    .open_subtitle = NULL,
    .open_data = NULL
};
