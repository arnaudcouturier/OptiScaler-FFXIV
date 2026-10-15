#pragma once
#include <unknwn.h>
// Replaces the product's Util.h for the files under test. Only the Streamline-unwrap seam is substituted.
namespace TestUtil
{
inline IUnknown* wrapped = nullptr;
inline IUnknown* real = nullptr;
} // namespace TestUtil
namespace Util
{
inline bool CheckForRealObject(const char*, IUnknown* object, IUnknown** real)
{
    if (object != TestUtil::wrapped)
        return false;
    *real = TestUtil::real;
    return true;
}
} // namespace Util
