#pragma once
// Test replacement of the DLSS-G preset status: the engine reads only whether the preset dropped the recomposition,
// which the test sets.
namespace ext::fg::preset
{
struct UiRecompositionDecision
{
    bool droppedByPreset = false;
};

inline UiRecompositionDecision& TestDecision()
{
    static UiRecompositionDecision decision;
    return decision;
}

inline UiRecompositionDecision CurrentUiRecompositionDecision(bool)
{
    return TestDecision();
}
} // namespace ext::fg::preset
