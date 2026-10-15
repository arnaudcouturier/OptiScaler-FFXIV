#pragma once

namespace ext::config
{
class Binder;
}

namespace ext
{
struct NrConfig
{
    void Bind(config::Binder&) {}
    void PostLoad() {}
};
} // namespace ext
