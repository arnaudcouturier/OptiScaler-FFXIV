#pragma once
// Developer probes of the output stream and the UI draws (docs/HUDLESS.md, "Developer probes"), runtime side. Everything here is log
// only and sits behind [HudLess] PlateDiagnostics (feature bit ext::F::PlateProbe, off by default). The seam bodies are in
// PlateProbe.cpp; the pure parts are PlateProbeAccounting.h, PlateProbeFormat.h and PlateProbeRing.h.

#include "ext/ExtFeatures.h"

#include <cstdint>
#include <string>

namespace ext::hudless::plate
{
// The probes are on: the key is set and no write has failed so far (a failure clears the feature bit).
inline bool Enabled() { return ext::Active(ext::F::PlateProbe); }

// Queued for the probe's own writer thread, which creates the run folder <game>\OptiScaler\plate-probe\<run>\ on first use and keeps the
// five newest runs. A failed write turns the probes off with one warning. Any thread; never blocks on the disk.
void AppendFile(const char* name, std::string text);
void RewriteFile(const char* name, std::string text);
} // namespace ext::hudless::plate
