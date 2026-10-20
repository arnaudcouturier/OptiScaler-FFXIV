#pragma once
#include <string>
namespace DlssNrNative {
void* WrapNvapi(unsigned id,void* original);
void SetEnabled(bool enabled);
void SetPrecision(unsigned precision);
// [DlssNr] VitCapture: log-only recording of the first NR evaluations' CUDA launches and the ViT module blobs
// (DlssNrVitCapture.h). Set from config load and before every NR evaluation; the module dump needs it set before
// the runtime creates its modules.
void SetCapture(bool enabled);
void BeginEvaluate(const void* feature, bool reset, unsigned every, bool allowUnverified = false);
void EndEvaluate(bool success = true);
// The stable policy recomputes downstream passes; disabling it restores all-pass reuse.
inline unsigned ReuseEveryForPass(unsigned pass, unsigned requestedEvery, bool firstPassOnly = true)
{
    return (!firstPassOnly || pass == 0) && requestedEvery > 1 ? 2u : 1u;
}
// The NR model file was switched (ext/nr/runtime): drop the reuse caches and the old verdict. Reuse stays off on the new module until its
// kernel blob is verified (or the override is on); the next unverified evaluation says so in the log.
void OnModelSwitch();
std::string VitStatus();
std::string Status();
}
