#pragma once
// Thread-id lines for the DX11 upscaler's lifecycle ([NgxRelease], ext/dx11bridge/NgxFeatureTable.h). They let the log show, after
// a session, which threads create, evaluate and release the feature, i.e. whether the narrow table lock and the closed/in-flight
// handling were needed. Formats and rate limiting: NgxThreadTraceCore.h. Cheap: one lock and a few compares per evaluate.

namespace ext::upscaler
{
// Once per feature create: `FFXIV NGX thread: create tid=<n> render=<w>x<h> display=<w>x<h>`.
void LogCreateThread(unsigned renderWidth, unsigned renderHeight, unsigned displayWidth, unsigned displayHeight);
// Per evaluate, rate limited. `key` tells evaluation sites apart (the immediate context pointer is enough).
void LogEvaluateThread(const void* key);
// Once per release: `FFXIV NGX thread: release handle=<n> tid=<n>` (called by ext::dx11bridge::BeforeReleaseFeature itself).
void LogReleaseThread(unsigned handleId);
} // namespace ext::upscaler
