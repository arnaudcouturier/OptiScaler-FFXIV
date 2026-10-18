#pragma once
// Stand-in for ext/core/ExtConfig.h in dynamic_mfg_config: a Binder that only records (section, key, field), so the real
// FgConfig::Bind can be compiled and its table inspected. The real CustomOptional comes from the sliced Config.h.
#include <Config.h>

#include <vector>

namespace ext::config
{
struct Entry
{
    const char* section;
    const char* key;
    void* field;
};

class Binder
{
  public:
    template <typename T, HasDefaultValue B>
    void Add(const char* section, const char* key, CustomOptional<T, B>& field, bool = false)
    {
        _table.push_back({ section, key, &field });
    }

    template <typename E, HasDefaultValue B>
    void AddEnum(const char* section, const char* key, CustomOptional<E, B>& field, E, bool = false)
    {
        _table.push_back({ section, key, &field });
    }

    const std::vector<Entry>& Table() const { return _table; }

  private:
    std::vector<Entry> _table;
};
} // namespace ext::config
