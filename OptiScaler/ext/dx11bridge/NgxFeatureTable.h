#pragma once
// The DX11 NGX feature table (`Dx11Contexts` in inputs/NVNGX_DLSS_Dx11.cpp) made safe for the threads that really use it, and the
// closed/in-flight registry that lets a release prove no Evaluate is still running on the handle. Pure, std only:
// tests/ext/dx11bridge/ngx_feature_table_test.cpp.
//
// Why: FFXIV changes quality on its update thread (the native resize callbacks release and re-create the upscaler there) while
// the render thread evaluates. Upstream keeps the table in an ankerl::unordered_dense::map, which is neither thread-safe nor
// reference-stable (an insert can reallocate, an erase moves the last element into the hole), and Evaluate holds a pointer into
// it for the whole call. So:
//   * ContextTable stores entries in node-based storage (references stay valid until THAT entry is extracted) and serialises
//     every structural operation under one narrow lock;
//   * Extract moves the entry out and erases it under the lock; the caller destroys it (the destructor calls NGX and the
//     driver) after the lock is gone;
//   * EvalRegistry counts the evaluates in flight per handle and can close a handle to new ones, so a release can wait for the
//     running ones with a bounded wait instead of the 500 ms sleep.
//
// LOCK ORDER. The table lock is a LEAF: while it is held nothing is called - no NGX/driver call, no GPU drain, no FG mutex, no NR
// lock (ext/nr g_nrMutex, DlssNrNative State::mutex), no feature destructor. NR runs inside the NGX calls and never calls back into
// the DX11 entry points, so "NR lock -> table lock" is the only direction that can occur and there is no cycle. The registry has its
// own mutex, also a leaf, never held together with the table lock or across a wait that calls out.

#include "ext/dx11bridge/ReleaseDrain.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace ext::dx11bridge
{
namespace detail
{
inline size_t ThisThreadKey() { return std::hash<std::thread::id> {}(std::this_thread::get_id()); }

// A mutex that knows its owner, so "never held across X" can be asserted.
class OwnedLeafMutex
{
  public:
    void lock()
    {
        _mutex.lock();
        _owner.store(ThisThreadKey(), std::memory_order_relaxed);
        _held.store(true, std::memory_order_relaxed);
    }
    void unlock()
    {
        _held.store(false, std::memory_order_relaxed);
        _mutex.unlock();
    }
    bool HeldByThisThread() const
    {
        return _held.load(std::memory_order_relaxed) && _owner.load(std::memory_order_relaxed) == ThisThreadKey();
    }

  private:
    std::mutex _mutex;
    std::atomic<size_t> _owner { 0 };
    std::atomic<bool> _held { false };
};

inline OwnedLeafMutex& TableMutex()
{
    static OwnedLeafMutex m;
    return m;
}
} // namespace detail

// The narrow lock itself (also used for the currentFeature swap). Not recursive: nothing may be called under it.
class TableGuard
{
  public:
    TableGuard() { detail::TableMutex().lock(); }
    ~TableGuard() { detail::TableMutex().unlock(); }
    TableGuard(const TableGuard&) = delete;
    TableGuard& operator=(const TableGuard&) = delete;
};

inline bool TableLockHeldByThisThread() { return detail::TableMutex().HeldByThisThread(); }

// Per-handle closed flag and in-flight evaluate count.
class EvalRegistry
{
  public:
    // False = the handle is closed (being released): the caller must not evaluate.
    bool TryEnter(unsigned id)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto& e = _entries[id];
        if (e.closed)
            return false;
        ++e.total;
        ++e.perThread[detail::ThisThreadKey()];
        return true;
    }

    // Pairs with a successful TryEnter. A handle forgotten in between is ignored.
    void Leave(unsigned id)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _entries.find(id);
            if (it == _entries.end())
                return;
            auto& e = it->second;
            if (e.total > 0)
                --e.total;
            auto t = e.perThread.find(detail::ThisThreadKey());
            if (t != e.perThread.end() && --t->second <= 0)
                e.perThread.erase(t);
        }
        _changed.notify_all();
    }

    void Close(unsigned id)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _entries[id].closed = true;
    }

    // A release that failed keeps the feature: it takes evaluates again.
    void Reopen(unsigned id)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _entries.find(id);
        if (it != _entries.end())
            it->second.closed = false;
    }

    // The handle is gone for good (entry extracted): a late Leave is a no-op and a late TryEnter finds an empty entry.
    void Forget(unsigned id)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _entries.erase(id);
        }
        _changed.notify_all();
    }

    // Waits (bounded) for the evaluates in flight on `id` to leave. Does not close the handle.
    Quiescence WaitIdle(unsigned id, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(_mutex);
        auto it = _entries.find(id);
        if (it == _entries.end())
            return Quiescence::Idle;
        if (it->second.perThread.count(detail::ThisThreadKey()) != 0)
            return Quiescence::SelfInside; // would wait for itself
        const bool idle = _changed.wait_for(lock, timeout, [&] {
            auto now = _entries.find(id);
            return now == _entries.end() || now->second.total == 0;
        });
        return idle ? Quiescence::Idle : Quiescence::EvaluateInFlight;
    }

    bool IsClosed(unsigned id) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _entries.find(id);
        return it != _entries.end() && it->second.closed;
    }
    int InFlight(unsigned id) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _entries.find(id);
        return it == _entries.end() ? 0 : it->second.total;
    }

  private:
    struct Entry
    {
        bool closed = false;
        int total = 0;
        std::map<size_t, int> perThread;
    };
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::unordered_map<unsigned, Entry> _entries;
};

inline EvalRegistry& Registry()
{
    static EvalRegistry r;
    return r;
}

// RAII for one Evaluate: `if (!pin) return` when the handle is closed.
class EvaluatePin
{
  public:
    EvaluatePin(EvalRegistry& registry, unsigned id) : _registry(registry), _id(id), _ok(registry.TryEnter(id)) {}
    explicit EvaluatePin(unsigned id) : EvaluatePin(Registry(), id) {}
    ~EvaluatePin()
    {
        if (_ok)
            _registry.Leave(_id);
    }
    explicit operator bool() const { return _ok; }
    EvaluatePin(const EvaluatePin&) = delete;
    EvaluatePin& operator=(const EvaluatePin&) = delete;

  private:
    EvalRegistry& _registry;
    unsigned _id;
    bool _ok;
};

// Replacement for `ankerl::unordered_dense::map<unsigned, V> Dx11Contexts`, with the three operations the file uses.
template <class V> class ContextTable
{
  public:
    explicit ContextTable(EvalRegistry& registry = Registry()) : _registry(registry) {}

    // Find or default-insert. The reference stays valid until this handle is extracted.
    V& operator[](unsigned id)
    {
        TableGuard lock;
        return _map[id];
    }

    // Moves the entry out and erases it under the lock (a missing handle gives an empty entry and inserts nothing), forgets the
    // handle in the registry, and returns. The caller destroys the returned entry with no lock held.
    V Extract(unsigned id)
    {
        V out {};
        {
            TableGuard lock;
            auto it = _map.find(id);
            if (it != _map.end())
            {
                out = std::move(it->second);
                _map.erase(it);
            }
        }
        _registry.Forget(id);
        return out;
    }

    size_t Size() const
    {
        TableGuard lock;
        return _map.size();
    }

    // Range-for under the lock: `for (const auto& [id, context] : table.Locked())`. The body may only read cheap state.
    class View
    {
      public:
        explicit View(std::unordered_map<unsigned, V>& map) : _map(map) {}
        auto begin() { return _map.begin(); }
        auto end() { return _map.end(); }

      private:
        TableGuard _lock;
        std::unordered_map<unsigned, V>& _map;
    };
    View Locked() { return View(_map); }

  private:
    // node-based on purpose: references to the other entries survive an insert or an erase
    std::unordered_map<unsigned, V> _map;
    EvalRegistry& _registry;
};
} // namespace ext::dx11bridge
