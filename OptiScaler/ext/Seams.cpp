#include "pch.h"

#include "ext/Seams.h"

// ext::Access definitions: the only fork code that names private upstream members (docs/SEAMS.md section 3.3).
// When upstream renames a member, this file is where the build breaks. Nothing is accessed yet; accessors are added
// together with the series that needs them.
namespace ext
{
} // namespace ext
