#include "pch.h"

#include "ext/hudless/adapters/Adapters.h"

namespace ext::hudless::adapters
{
// FSR-FG (UNTESTED here, docs/HUDLESS.md 9.2): the hud-less image goes into the FFX frame generation config through the
// provider's SetResource, exactly as the reference build did. Its UI resource is never registered, so no UI image is
// built in recomposition mode. There is no UI-free base and no paste for this provider.
TagResult FsrFgTag(IFGFeature_Dx12& fg, const TagContext& context) { return detail::Tag(fg, context, false); }
} // namespace ext::hudless::adapters
