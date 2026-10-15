#pragma once

namespace ext::config
{
class Binder;
}

namespace ext
{
struct FgConfig
{
    void Bind(config::Binder&) {}
    void PostLoad() {}
};
} // namespace ext
