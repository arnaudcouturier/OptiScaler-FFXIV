#pragma once
// The only fork header upstream files may include (docs/SEAMS.md section 1).
// Every seam is a free function in `ext::<group>`; the catalog is docs/SEAMS-catalog.md.
// Hot-path seams are inline wrappers testing a feature bit, see ExtFeatures.h.

#include "ext/ExtFeatures.h"
#include "ext/core/MenuRegistry.h"

#include <cstddef>

// CSimpleIniA is a typedef of a template, it cannot be forward declared
#include <SimpleIni.h>

namespace ext
{
// Friend of the upstream classes whose private state the fork has to reach (`friend struct ext::Access;`).
// It is the only fork code that names private upstream members. Accessors are added next to the seam that needs
// them; the definitions live in Seams.cpp. (The menu seam needs none: it is a template, see ext/core/MenuRegistry.h.)
struct Access
{
};

// ini load/save hooks in Config.cpp (state: ext/core/ExtConfig)
namespace config
{
void Load(CSimpleIniA& ini);    // end of Config::Reload, after upstream read its keys
void Save(CSimpleIniA& ini);    // Config::SaveIni, before ini.SaveFile
void SaveXeFG(CSimpleIniA& ini); // Config::SaveXeFG, before ini.SaveFile
} // namespace config

// menu_common.cpp (state: ext/core/MenuRegistry). ext::menu::MergeBoxes(base, count) is a template declared in
// ext/core/MenuRegistry.h: `base` is upstream's static MenuBox array, the result is `base` itself while no fork card is
// registered, otherwise a merged array (count is updated).

// core services (state: ext/core)
namespace core
{
void Banner(); // once, right after upstream logs its version line
} // namespace core

// Groups of later series. Declarations are added here together with the SEAMS-catalog.md row.
namespace upscaler {}
namespace dx11bridge {}
namespace nr {}
namespace fg {}
namespace present {}
namespace restrack {}
namespace streamline {}
namespace gpu {}
namespace hdr {}
namespace hudless {}
namespace game {}
} // namespace ext
