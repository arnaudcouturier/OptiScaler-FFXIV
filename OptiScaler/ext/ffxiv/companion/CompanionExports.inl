extern "C"
{
__declspec(dllexport) int __cdecl OptiScalerCompanion_BeginNamePlateV1(uint64_t session)
{
    ext::ffxiv::companion::Status status;
    auto callback = ext::ffxiv::companion::beginNativeDraw.load();
    if (!callback || !ext::ffxiv::companion::mailbox.Query(session, status, ext::ffxiv::companion::Now())) return 0;
    callback();
    return 1;
}
__declspec(dllexport) void __cdecl OptiScalerCompanion_EndNamePlateV1(uint64_t session)
{
    ext::ffxiv::companion::Status status;
    auto callback = ext::ffxiv::companion::endNativeDraw.load();
    if (callback && ext::ffxiv::companion::mailbox.Query(session, status, ext::ffxiv::companion::Now())) callback();
}
__declspec(dllexport) uint64_t __cdecl OptiScalerCompanion_OpenV1(uint32_t version, uint32_t frameSize,
    uint32_t plateSize, uint32_t statusSize, int64_t frequency)
{
    return ext::ffxiv::companion::mailbox.Open(version, frameSize, plateSize, statusSize, frequency,
                                      ext::ffxiv::companion::Frequency(), ext::ffxiv::companion::Now());
}
__declspec(dllexport) int __cdecl OptiScalerCompanion_SubmitV1(uint64_t session,
    const ext::ffxiv::companion::Frame* frame, const ext::ffxiv::companion::Plate* plates, uint32_t bytes)
{
    if (!frame) return 0;
    return ext::ffxiv::companion::mailbox.Submit(session, *frame, plates, bytes, ext::ffxiv::companion::Now()) ? 1 : 0;
}
__declspec(dllexport) int __cdecl OptiScalerCompanion_QueryV1(uint64_t session,
    ext::ffxiv::companion::Status* status, uint32_t bytes)
{
    if (!status || bytes != sizeof(*status)) return 0;
    return ext::ffxiv::companion::mailbox.Query(session, *status, ext::ffxiv::companion::Now()) ? 1 : 0;
}
__declspec(dllexport) void __cdecl OptiScalerCompanion_CloseV1(uint64_t session)
{
    ext::ffxiv::companion::mailbox.Close(session);
}
}
