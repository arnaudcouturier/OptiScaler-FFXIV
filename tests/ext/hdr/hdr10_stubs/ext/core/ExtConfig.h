#pragma once

#include "ext/hdr/HdrConfig.h"

namespace ext
{
struct TestExtConfig
{
    HdrConfig hdr;
};

inline TestExtConfig& Cfg()
{
    static TestExtConfig value;
    return value;
}
} // namespace ext
