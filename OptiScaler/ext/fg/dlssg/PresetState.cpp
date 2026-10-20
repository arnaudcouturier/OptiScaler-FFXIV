#include "pch.h"

#include "ext/core/ExtConfig.h"
#include "ext/fg/dlssg/PresetRequest.h"

namespace ext::fg::preset
{
// The request state of the process. The configured [DLSSG] RenderPreset becomes the first active request, read when this is first
// called (config is loaded long before NVIDIA's runtime asks for its settings).
RequestState& Requests()
{
    static RequestState state;
    state.InitOnce(ext::Cfg().fg.renderPreset.value_or_default()); // first call wins
    return state;
}
} // namespace ext::fg::preset
