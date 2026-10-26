#include "pch.h"

#include "ext/ffxiv/catalog/FfxivCatalog.h"

namespace ext::ffxiv::catalog
{
// Called from ext::ffxiv::ui::OnPresentBegin on every bridge Present. Cost when nothing is due: one relaxed add, one flag exchange and six
// loads (Ledger::Poll returns an empty vector without allocating). The lines (a handful, a few times per session) are written here, never
// from the shader creation path.
void OnPresent() noexcept
{
    try
    {
        PresentTick(Global(), ElapsedMs(), [](const std::string& line) { LOG_INFO("{}", line); });
    }
    catch (...)
    {
    }
}
} // namespace ext::ffxiv::catalog
