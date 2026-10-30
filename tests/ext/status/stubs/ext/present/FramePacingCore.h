#pragma once
// Test stand-in for the present phases: only the values the status writer
// filters on are modelled.

namespace ext::present
{
enum class Phase : unsigned
{
    NativePresent,
    HiddenPresent,
    FgDispatch,
    ReflexSleep,
    OutputPresent,
    SceneNr,
    PreviewNr,
    Count
};
} // namespace ext::present
