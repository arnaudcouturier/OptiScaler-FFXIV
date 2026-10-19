#pragma once
// The one place where the guides the super-resolution evaluate is given (CharaDlssNgx.h, EvaluateGuidesOf) become the guides of a production
// NR run (CharaNrRecipe.h, GuideSetup). Both come from the same EvaluateInputs of the same render, so the NR model is told the render size and
// the motion scale the upscaler itself was told, and a run cannot be handed the output size by passing a wrong width (the colour image is the
// only one at output size).
//
// A separate header because CharaDlssNgx.h (the upscaler's parameters) and CharaNrRecipe.h (the NR passes) should not include each other.

#include "ext/ffxiv/chara/CharaDlssNgx.h"
#include "ext/ffxiv/chara/CharaNrRecipe.h"

namespace ext::ffxiv::chara::nr
{
// The guides of a production run for the render the super-resolution evaluate `in` was made for: the depth and motion images have the render
// size, the depth is inverted when the feature of the evaluate was created that way (`reversedZ`, the slot's convention), and the motion
// vectors (fractions of the render image) are scaled by the render size, the same scale the evaluate is given.
constexpr GuideSetup GuidesOf(const dlss::ngx::EvaluateInputs& in, bool reversedZ)
{
    const dlss::ngx::EvaluateGuides guides = dlss::ngx::EvaluateGuidesOf(in);
    return GuideSetup { guides.width, guides.height, reversedZ, guides.mvScaleX, guides.mvScaleY };
}
} // namespace ext::ffxiv::chara::nr
