#pragma once
// The NR runtime selection: one immutable snapshot with a revision, and the rule that turns a stream of menu clicks into a few
// switches. Pure (no file access, no clock of its own): the resolver and the time are handed in, so
// tests/ext/nr/nr_runtime_selection.cpp drives it with a fake catalog and a fake clock.
//
// The consumers (the scene pass, the character previews) hold a std::shared_ptr<const Snapshot> and compare `revision` with the one
// they have built for; a different revision means "load another model".

#include "ext/nr/runtime/NrRuntimeCatalog.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace ext::nr::runtime
{
struct Snapshot
{
    uint64_t revision = 0;
    std::wstring requested; // the request text this was resolved from ("" = Automatic)
    Status status = Status::NothingInstalled;
    std::wstring id;         // "legacy" or the folder name; empty when there is no model
    std::filesystem::path path;
    bool legacy = false;
    VersionParts version;    // OrderVersion of the chosen entry

    bool Available() const { return status == Status::Ok && !path.empty(); }

    // Two snapshots name the same model when they agree on this; the revision only moves when it changes.
    // Without a model the request is part of the identity: it is what the "not installed" message names.
    bool SameModel(const Snapshot& o) const
    {
        return status == o.status && id == o.id && path == o.path && (Available() || requested == o.requested);
    }
};

// Why there is no model, in the words the card and the log use.
inline std::string UnavailableReason(const Snapshot& s)
{
    auto narrow = [](const std::wstring& w)
    {
        std::string out;
        for (const wchar_t c : w)
            out += (c > 0 && c < 128) ? static_cast<char>(c) : '?';
        return out;
    };
    switch (s.status)
    {
    case Status::Ok: return {};
    case Status::NothingInstalled: return "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
    case Status::ExplicitMissing:
        return "the selected NR runtime " + narrow(s.requested.empty() ? std::wstring(L"auto") : s.requested) +
               " is not installed; nothing else is loaded in its place";
    case Status::InvalidName:
        return "'" + narrow(s.requested) + "' is not a valid NR runtime (use auto, legacy or a folder name like NR.310.9)";
    }
    return {};
}

// What a request resolves to against the files that exist. The revision is added by the Selector.
inline Snapshot MakeSnapshot(const std::vector<Entry>& entries, std::wstring_view requested)
{
    const Resolution r = Resolve(entries, requested);
    Snapshot s;
    s.requested = std::wstring(detail::Trim(requested));
    s.status = r.status;
    if (r.status == Status::Ok && r.index)
    {
        const Entry& e = entries[*r.index];
        s.id = e.id;
        s.path = e.path;
        s.legacy = e.legacy;
        s.version = OrderVersion(e);
    }
    return s;
}

// Holds the current snapshot. Poll() is called every frame with the request as it stands now (the config value); the first call
// resolves at once, a later change is applied only after `debounceMs` without another change (rapid clicks become one switch), and
// the revision moves only when the chosen model really differs.
class Selector
{
  public:
    using ResolveFn = std::function<Snapshot(std::wstring_view requested)>;

    explicit Selector(ResolveFn resolve, uint64_t debounceMs = 250) : _resolve(std::move(resolve)), _debounceMs(debounceMs) {}

    std::shared_ptr<const Snapshot> Poll(std::wstring_view wanted, uint64_t nowMs)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_current)
        {
            Install(_resolve(wanted));
            _wanted = std::wstring(wanted);
            _pending = false;
            _rescan = false;
            return _current;
        }
        if (wanted != _wanted)
        {
            _wanted = std::wstring(wanted);
            _changedAt = nowMs;
            _pending = true;
        }
        if (_pending)
        {
            if (nowMs - _changedAt >= _debounceMs)
            {
                _pending = false;
                _rescan = false;
                Install(_resolve(_wanted));
            }
        }
        else if (_rescan)
        {
            _rescan = false;
            Install(_resolve(_wanted));
        }
        return _current;
    }

    // The files on disk may have changed (the card's rescan): resolve the request again at the next Poll.
    void Rescan()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _rescan = true;
    }

    // A request was seen and waits for the quiet period.
    bool Pending() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _pending;
    }

    std::shared_ptr<const Snapshot> Current() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _current;
    }

  private:
    void Install(Snapshot next)
    {
        if (_current && _current->SameModel(next))
            return; // same model: no new revision, nobody reloads anything
        next.revision = _current ? _current->revision + 1 : 1;
        _current = std::make_shared<const Snapshot>(std::move(next));
    }

    ResolveFn _resolve;
    uint64_t _debounceMs;
    mutable std::mutex _mutex;
    std::shared_ptr<const Snapshot> _current;
    std::wstring _wanted;
    uint64_t _changedAt = 0;
    bool _pending = false;
    bool _rescan = false;
};
} // namespace ext::nr::runtime
