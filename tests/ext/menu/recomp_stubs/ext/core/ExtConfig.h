#pragma once
// Stand-in for ext/core/ExtConfig.h in ui_recomposition_roundtrip: a Binder that keeps, per bool field, the same Save and
// Load steps as the real Binder (ExtConfig.h detail::Save / detail::Load for bool: FormatBool(value_for_config()) and
// set_from_config(ParseBool(raw))). The test pins those two real lines by text, so this copy cannot drift silently. The
// real CustomOptional comes from the sliced Config.h that prepare_dynamic_mfg_config.ps1 writes.
#include <Config.h>

#include <string>
#include <vector>

namespace ext::config
{
struct Entry
{
    const char* section;
    const char* key;
    void* field;
    std::string (*save)(void*); // what Save Settings writes ("" for a field this stub does not model)
    void (*load)(void*, const std::string&); // what Load does with a raw ini value (lower case) ("auto" = unset)
};

class Binder
{
  public:
    template <typename T, HasDefaultValue B>
    void Add(const char* section, const char* key, CustomOptional<T, B>& field, bool = false)
    {
        Entry e { section, key, &field, nullptr, nullptr };
        if constexpr (std::is_same_v<T, bool>)
        {
            e.save = [](void* f) -> std::string
            {
                const auto v = static_cast<CustomOptional<bool, B>*>(f)->value_for_config();
                return !v ? "auto" : (*v ? "true" : "false");
            };
            e.load = [](void* f, const std::string& raw)
            {
                std::optional<bool> parsed;
                if (raw == "true")
                    parsed = true;
                else if (raw == "false")
                    parsed = false;
                static_cast<CustomOptional<bool, B>*>(f)->set_from_config(parsed);
            };
        }
        _table.push_back(e);
    }

    template <typename E, HasDefaultValue B>
    void AddEnum(const char* section, const char* key, CustomOptional<E, B>& field, E, bool = false)
    {
        _table.push_back({ section, key, &field, nullptr, nullptr });
    }

    const std::vector<Entry>& Table() const { return _table; }

  private:
    std::vector<Entry> _table;
};
} // namespace ext::config
