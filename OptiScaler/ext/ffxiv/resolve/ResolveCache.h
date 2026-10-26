#pragma once
// ResolveOnce: resolve everything exactly once, cache the result for the life of the process, never scan lazily or again.
//
// THE CONTRACT FOR THE CALLER (ResolveStartup.cpp follows it in the product):
//   * call it ONCE, early, BEFORE the fork installs any Detours on the game's functions (the resolver reads the unpatched code; a hook
//     already in place would hide the very bytes the patterns look for, apart from the EntryPatched case it reports),
//   * NOT under the loader lock (not from DllMain): the scan reads the whole code section of a 50 MB executable,
//   * from one thread (other threads that call it meanwhile wait for the first to finish, then see the same result),
//   * the sink (options.sink) must not call ResolveOnce / Get again: it runs inside the once-flag and would deadlock.
// The result is never recomputed: the process image does not change, and a second scan could only disagree with the first.
// A failure (an exception from the image reader, bad_alloc) is cached too: every seam NotFound, the reason in registryErrors.

#include "Resolver.h"

#include <atomic>
#include <exception>
#include <mutex>

namespace ext::ffxiv::resolve
{
class ResolveCache
{
  public:
    // True once Get has produced a result (also a cached failure).
    bool Resolved() const { return _resolved.load(std::memory_order_acquire); }

    // The first call resolves (and logs, through options.sink); every later call returns the same object whatever its arguments.
    const ResolvedBuild& Get(const ImageView& image, const Registry& registry, const ResolveOptions& options = {})
    {
        std::call_once(_once,
                       [&]
                       {
                           try
                           {
                               _result = ResolveImage(image, registry, options);
                           }
                           catch (const std::exception& e)
                           {
                               _result = Failed(image, registry, options, e.what());
                           }
                           catch (...)
                           {
                               _result = Failed(image, registry, options, "unknown error");
                           }
                           _resolved.store(true, std::memory_order_release);
                       });
        return _result;
    }

  private:
    // The value of a failed resolve: every seam NotFound and, like ResolveImage's result, every Rva / Guard field ZERO (ResolvedBuild starts
    // zeroed: never the C table value). The stamp and size of the image are copied when they can be read. One Error line says why.
    static ResolvedBuild Failed(const ImageView& image, const Registry& registry, const ResolveOptions& options, const char* what)
    {
        ResolvedBuild rb;
        try
        {
            try
            {
                rb.timeDateStamp = image.TimeDateStamp();
                rb.sizeOfImage = image.SizeOfImage();
                rb.build.timeDateStamp = rb.timeDateStamp;
                rb.build.sizeOfImage = rb.sizeOfImage;
            }
            catch (...)
            {
            }
            for (const SeamSpec& s : registry.seams)
            {
                SeamResult r;
                r.id = s.id;
                r.reason = "the resolver stopped with an error";
                rb.seams.push_back(std::move(r));
            }
            rb.registryErrors.push_back(std::string("resolver exception: ") + what);
            if (options.sink)
                options.sink(LogLevel::Error, "[ffxiv-resolve] resolver stopped with an error, nothing resolved: " + std::string(what));
        }
        catch (...)
        {
        }
        return rb;
    }

    std::once_flag _once;
    ResolvedBuild _result;
    std::atomic<bool> _resolved { false };
};

// The process-wide instance.
inline const ResolvedBuild& ResolveOnce(const ImageView& image, const Registry& registry, const ResolveOptions& options = {})
{
    // Deliberately leaked: game threads may still hold the returned reference while the process shuts down and static destructors run.
    static ResolveCache& cache = *new ResolveCache;
    return cache.Get(image, registry, options);
}
} // namespace ext::ffxiv::resolve
