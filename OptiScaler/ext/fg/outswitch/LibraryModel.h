#pragma once
// The measured release model table: which class, on which library build, is M1 or M2 (SwitchTypes.h), and whether that
// library honours a private-data interface on the outer chain.
//
// Why a table and not a rule: the two release paths in the code contradict each other (one releases the chain after the
// provider's destroy, the other relies on the destroy to end it), and neither library's destroy is visible in source.
// The separate probe process (tools/fg-probe) measures both questions per class and per library build, and the switch
// refuses a build the table does not know. Fail closed: an unknown key is NoModel, never a guess.
//
// Only compiled-in rows decide a switch: Seeded() is built from ModelRows.h, which
// tools/fg-probe/Update-ModelTable.ps1 regenerates from the probe's data file. A probe run in the game process
// (Probe.cpp) logs what it sees and nothing else: its rows are never consulted, so one noisy run cannot authorise a
// release rule. A new library build is a new probe run and a new row, not a new decision at run time.
//
// Pure: no Windows, DXGI or upstream include (tests/ext/fg/fg_switch_model.cpp).

#include "ext/fg/outswitch/ModelRows.h"
#include "ext/fg/outswitch/SwitchTypes.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace ext::fg::outswitch
{
// What identifies a library build: the file name, its size in bytes and the PE header's TimeDateStamp. All three are read
// from the loaded module without an extra import (the image headers are mapped), which is why the product does not link
// version.dll just to tell two builds apart. A rebuild that changes none of them behaves identically, which is what the
// table assumes.
struct LibraryIdentity
{
    char file[64] {};
    uint64_t size = 0;
    uint32_t stamp = 0; // IMAGE_FILE_HEADER.TimeDateStamp of the module the chain's present slot lives in

    bool Empty() const { return file[0] == '\0' || size == 0; }
};

inline bool SameLibrary(const LibraryIdentity& a, const LibraryIdentity& b)
{
    return a.size == b.size && a.stamp == b.stamp && std::strncmp(a.file, b.file, sizeof(a.file)) == 0;
}

// "amd_fidelityfx_framegeneration_dx12.dll size=12345678 stamp=65f1a2b3" for the log and the probe data.
inline void IdentityText(const LibraryIdentity& id, char (&out)[160])
{
    std::snprintf(out, sizeof(out), "file=%s size=%llu stamp=%08X", id.file,
                  static_cast<unsigned long long>(id.size), id.stamp);
}

// The configuration a row was measured in, and the only one that authorises a release rule for a class:
//   direct  the frame-generation library called directly, as the FSR FG path does (the product's loader is a forwarder to
//           the same DLL, and the release patch only changes who calls Release)
//   xell    the frame-generation context with an XeLL context attached through the library's own latency-reduction call,
//           which is the configuration the product always runs for XeFG
// A row measured in another configuration describes a setup the product never runs (an XeFG context without XeLL holds no
// reference for XeLL to keep, which is exactly the difference the pin release depends on), so it cannot be used.
inline bool ConfigUsable(Class cls, const char* config)
{
    if (config == nullptr)
        return false;
    if (cls == Class::XeFg)
        return std::strcmp(config, "xell") == 0;
    if (cls == Class::FsrFg)
        return std::strcmp(config, "direct") == 0;
    return false;
}

// The configuration token for a class, for the host's row line and the data file.
inline const char* ConfigText(Class cls) { return cls == Class::XeFg ? "xell" : "direct"; }

struct ModelRow
{
    Class cls = Class::Plain;
    LibraryIdentity library;
    // The companion DLL the measured configuration needs: libxell.dll for XeFG (the XeLL context the product attaches).
    // A different companion is a different build of the pair the row measured, so it is part of the identity, not a
    // note: a player with another libxell.dll next to the same libxess_fg.dll gets no row and a refused switch.
    LibraryIdentity companion;
    LibraryModel model;
    // The order the row was measured with. false: the library's destroy first, then the caller's reference. true: the
    // caller's reference first, then exactly one destroy - the order that frees a chain whose provider context refuses to
    // be destroyed while somebody else still holds the chain (XeFG). A row authorises its own order only.
    bool pinFirst = false;
    char config[16] {}; // what the row was measured with: "direct" (FSR FG) or "xell" (XeFG with its XeLL context)
    uint32_t standstillRefs = 0; // the chain's reference count at standstill, the same in every measured run
    // What the same runs said about the XeFG context and the other teardown order, and what a leaked context cost in
    // local video memory. Carried as evidence, never as a decision: the model above is decided by the window and the
    // witness only.
    char context[16] {}; // "destroyed" (a second destroy freed the context), "leaked", "unknown"
    char orderB[16] {};  // "clean" (release then destroy leaves the window free), "leaked", "unknown"
    int32_t vramDeltaMb = 0;

    bool Usable() const
    {
        if (cls == Class::Plain || library.Empty() || model.model == Model::Unknown || !ConfigUsable(cls, config) ||
            standstillRefs == 0)
            return false;
        // M2 is the direction that touches freed memory if it is wrong: the pin release after the destroy. It is only
        // ever admitted with a witness the library accepted, which is what the teardown cross-checks the release against.
        // A row written by hand (or copied from an older probe line) without one is not a row.
        if (model.model == Model::M2 && !model.witnessHonoured)
            return false;
        // A row only authorises the order it was measured with, and this order is a release of a reference that must
        // then be followed by the destroy: a row that is not M2 has no reference for it to release.
        if (pinFirst && (model.model != Model::M2 || !model.witnessHonoured))
            return false;
        // An XeFG row was measured with an XeLL context attached: the build of that context is part of the pair.
        if (cls == Class::XeFg && companion.Empty())
            return false;
        return true;
    }
};

// The same question the table asks: is this live library (and, for XeFG, this live companion) the pair the row measured?
// `companion` is empty when the caller could not name it, which is a refusal for XeFG and irrelevant for FSR FG.
inline bool SameRow(const ModelRow& row, const LibraryIdentity& library, const LibraryIdentity& companion)
{
    if (!SameLibrary(row.library, library))
        return false;
    if (row.cls == Class::XeFg)
        return !companion.Empty() && SameLibrary(row.companion, companion);
    return true;
}

// One measured run of the probe host: what the window looked like after the destroy, and after one pin release.
struct ProbeRun
{
    bool windowFreeAfterDestroy = false;   // every read in the polling window agreed the window was free
    bool windowStableAfterDestroy = false; // the reads settled on an answer (a continuous quiet period, no flapping)
    bool windowFreeAfterPin = false;
    bool windowStableAfterPin = false;
    // The private-data interface was still registered when the quiet window ended, i.e. before the pin release. Sampled
    // there on purpose: under M2 the release is what frees the object, so a sample taken after it always reads "gone" and
    // would throw every M2 measurement away. M1 expects this false (the destroy freed the object), M2 expects it true in
    // every run.
    bool witnessAliveBeforePin = false;
    // The witness after the single destroy of a pin-first run: it must be gone. Still registered means the library's
    // destroy did not free the object (it refused, or a context kept it), which is a leaked chain and not a row.
    bool witnessAliveAfterDestroy = false;
    // The order this run measured: false = destroy, then the caller's reference; true = the caller's reference, then one
    // destroy. A row carries the order it was measured with and is only used for that one.
    bool pinFirst = false;
    bool releaseSkipped = false; // the witness reported the object gone, so the pin was NOT released
    // The second destroy, after the pin release: a context that refused while the chain was still held (XeFG answers
    // POINTER_STILL_IN_USE) can be destroyed now. 0 = the library answered success. Only XeFG reports it.
    uint32_t destroyAfterPin = 0;
    bool destroyAfterPinAttempted = false;
    // The other order measured on the same class: release the pin first, then destroy. True = the window came free (the
    // order is leak free), false = the window stayed bound (the destroy still holds something).
    bool orderBMeasured = false;
    bool orderBFree = false;
    // Local video memory in use when the run started and after its teardown, in MiB: a leaked context shows up here.
    int64_t vramDeltaMb = 0;
    // The chain's reference count at standstill (AddRef, read, Release) before anything is destroyed.
    uint32_t standstillRefs = 0;
    // The library's answer to its own destroy call. A refusal (XeFG answers POINTER_STILL_IN_USE while the bridge still
    // holds the chain) is what makes that context survive the switch path: the chain is freed by the pin release, and the
    // context that refused to destroy it has nobody left to destroy it.
    int32_t firstDestroyCode = 0;
};

// A classification needs three consistent runs: one read, or one run, is what a library that tears down on its own
// thread makes wrong in the dangerous direction.
inline constexpr size_t kProbeRuns = 3;

struct ProbeRuns
{
    bool ran = false;
    ProbeRun runs[kProbeRuns] {};
    size_t runCount = 0;
    bool witnessAttempted = false;
    bool witnessAccepted = false;
    bool witnessReleasedWithOuter = false;
};

struct Classified
{
    LibraryModel model;
    bool pinFirst = false;
    const char* evidence = "";
};

// The classification, kept here so the host, the in-game reader and the tests agree on one rule:
//   M1  every run saw the window free after the destroy (with settled reads): the destroy ended the object, so the pin
//       release must not happen;
//   M2  every run saw the window still bound after the destroy and free after exactly one pin release (with settled
//       reads), and - when a witness was accepted - that witness alive at the end of the quiet window in every run: the
//       context held a reference and the pin release frees the object;
//   anything else is Unknown, which the switch refuses. A noisy measurement is never turned into a row, and M2 - the
//       dangerous direction on an M1 library - needs every run to agree on both signals. A run whose witness reported the
//       object gone before the pin release is unusable (the release was skipped), and a witness that contradicts the
//       window makes the whole measurement unknown.
inline Classified ClassifyProbeRuns(const ProbeRuns& runs)
{
    Classified out;
    out.model.witnessHonoured = runs.witnessAttempted && runs.witnessAccepted && runs.witnessReleasedWithOuter;
    if (!runs.ran || runs.runCount < kProbeRuns)
    {
        out.evidence = "not enough runs";
        return out;
    }

    // A witness the library refused is not a witness: the row would carry witness=no and the teardown would have no
    // cross-check at all, so the measurement is unusable whatever the window says.
    if (runs.witnessAttempted && !runs.witnessAccepted)
    {
        out.evidence = "the library refused the release witness";
        return out;
    }

    // The runs of one measurement have one order. A mixed set is two measurements, not one row.
    const bool pinFirst = runs.runs[0].pinFirst;
    for (size_t i = 1; i < runs.runCount; ++i)
    {
        if (runs.runs[i].pinFirst != pinFirst)
        {
            out.evidence = "the runs were measured in different orders";
            return out;
        }
    }

    if (pinFirst)
    {
        // The caller's reference first, then exactly one destroy. A row in this order needs: the witness accepted, alive
        // after the reference was released (the context held it, which is what makes the release safe), and gone after
        // the single destroy; the window free and settled; the library's own destroy success; and no second destroy
        // anywhere (a chain that needs one was not measured in this order).
        if (!(runs.witnessAttempted && runs.witnessAccepted && runs.witnessReleasedWithOuter))
        {
            out.evidence = "a pin-first row needs a release witness the library accepted and that died with the object";
            return out;
        }
        for (size_t i = 0; i < runs.runCount; ++i)
        {
            const ProbeRun& run = runs.runs[i];
            if (run.releaseSkipped)
            {
                out.evidence = "the witness reported the object gone before the destroy";
                return out;
            }
            if (!run.windowStableAfterDestroy || !run.windowFreeAfterDestroy)
            {
                out.evidence = "the window was not free after the single destroy in every run";
                return out;
            }
            if (!run.witnessAliveBeforePin)
            {
                out.evidence = "the release of the caller's reference ended the object";
                return out;
            }
            if (run.witnessAliveAfterDestroy)
            {
                out.evidence = "the destroy did not free the object (the witness was still there)";
                return out;
            }
            if (run.firstDestroyCode != 0)
            {
                out.evidence = "the library's destroy did not answer success";
                return out;
            }
            if (run.destroyAfterPinAttempted)
            {
                out.evidence = "a second destroy was attempted, so the order was not pin first alone";
                return out;
            }
        }
        out.model.model = Model::M2;
        out.model.witnessHonoured = true;
        out.pinFirst = true;
        out.evidence = "the reference was released first and the single destroy freed the object in every run";
        return out;
    }

    size_t free = 0;
    size_t bound = 0;
    size_t witnessAlive = 0;
    const bool witnessSeen = runs.witnessAttempted && runs.witnessAccepted;
    for (size_t i = 0; i < runs.runCount; ++i)
    {
        const ProbeRun& run = runs.runs[i];
        if (run.releaseSkipped)
        {
            out.evidence = "the witness reported the object gone before the pin release";
            return out;
        }
        if (!run.windowStableAfterDestroy || (run.windowFreeAfterPin && !run.windowStableAfterPin))
        {
            out.evidence = "the window reads did not settle";
            return out;
        }
        if (run.windowFreeAfterDestroy)
            ++free;
        else
            ++bound;
        if (run.witnessAliveBeforePin)
            ++witnessAlive;
    }

    if (free == runs.runCount)
    {
        // M1 needs the witness to have died with the object when one was accepted: a live witness on a free window is a
        // contradiction, not a measurement.
        if (witnessSeen && witnessAlive != 0)
        {
            out.evidence = "the witness outlived a destroy that freed the window";
            return out;
        }
        out.model.model = Model::M1;
        out.evidence = "the window was free after the destroy in every run";
        return out;
    }

    if (bound == runs.runCount)
    {
        bool freedAfterPin = true;
        for (size_t i = 0; i < runs.runCount; ++i)
            if (!runs.runs[i].windowFreeAfterPin)
                freedAfterPin = false;
        // M2 is the dangerous direction on an M1 library: it needs the window free after the pin AND a witness the
        // library accepted, alive in every run and released with the outer chain. Without an accepted witness the window
        // alone decided, which is exactly the guess this table must never make, so the verdict is unknown.
        if (!witnessSeen)
        {
            out.evidence = "M2 needs a release witness the library accepted, and none was placed";
            return out;
        }
        if (!runs.witnessReleasedWithOuter)
        {
            out.evidence = "the witness outlived the outer chain (a forwarding wrapper)";
            return out;
        }
        if (freedAfterPin && witnessAlive == runs.runCount)
        {
            out.model.model = Model::M2;
            out.evidence = "the window stayed bound until the pin release in every run";
            return out;
        }
        if (!freedAfterPin)
        {
            out.evidence = "the window stayed bound after the pin release";
            return out;
        }
        out.evidence = "the witness did not stay alive in every run";
        return out;
    }

    out.evidence = "the runs disagree";
    return out;
}

// Rows the probe has recorded in ModelRows.h. Empty until a measurement is written down: the switch then refuses every
// provider class with NoModel instead of guessing a release rule.
class ModelTable
{
  public:
    static constexpr size_t kMaxRows = 16;

    // False when the table is full or the row is unusable (no file name, no model).
    bool Add(const ModelRow& row)
    {
        if (!row.Usable() || count_ >= kMaxRows)
            return false;
        for (size_t i = 0; i < count_; ++i)
            if (rows_[i].cls == row.cls && SameRow(rows_[i], row.library, row.companion))
                return false; // a build has one answer; a second probe does not overwrite the first
        rows_[count_++] = row;
        return true;
    }

    // False = no row for this class and build: the caller refuses (NoModel), it never assumes a model.
    bool Find(Class cls, const LibraryIdentity& library, LibraryModel& out) const
    {
        // Without the companion identity an XeFG row cannot be confirmed, so the caller that could not name it gets the
        // fail-closed answer (SameRow with an empty companion).
        return Find(cls, library, LibraryIdentity {}, out);
    }

    bool Find(Class cls, const LibraryIdentity& library, const LibraryIdentity& companion, LibraryModel& out) const
    {
        if (cls == Class::Plain || library.Empty())
            return false;
        for (size_t i = 0; i < count_; ++i)
        {
            if (rows_[i].cls == cls && SameRow(rows_[i], library, companion))
            {
                out = rows_[i].model;
                out.pinFirst = rows_[i].pinFirst;
                out.standstillRefs = rows_[i].standstillRefs;
                return out.model != Model::Unknown;
            }
        }
        return false;
    }

    size_t Count() const { return count_; }
    const ModelRow& At(size_t index) const { return rows_[index]; }

  private:
    ModelRow rows_[kMaxRows] {};
    size_t count_ = 0;
};

// The compiled-in rows. Every entry must come from a probe run written down in the probe data file
// (tools/fg-probe/model-rows.txt, regenerated by Update-ModelTable.ps1); this list is the only place the switch learns a
// model. The generated placeholder row (an empty file name) is skipped.
inline const ModelTable& Seeded()
{
    static const ModelTable table = [] {
        ModelTable built;
        for (size_t i = 0; i < rows::kRowCount; ++i)
        {
            const rows::Row& row = rows::kRows[i];
            if (row.file == nullptr || row.file[0] == '\0')
                continue;
            ModelRow entry;
            entry.cls = row.cls;
            std::snprintf(entry.library.file, sizeof(entry.library.file), "%s", row.file);
            entry.library.size = row.size;
            entry.library.stamp = row.stamp;
            if (row.companionFile != nullptr && row.companionFile[0] != '\0')
            {
                std::snprintf(entry.companion.file, sizeof(entry.companion.file), "%s", row.companionFile);
                entry.companion.size = row.companionSize;
                entry.companion.stamp = row.companionStamp;
            }
            entry.model = LibraryModel { row.model, row.witnessHonoured };
            entry.pinFirst = row.pinFirst;
            entry.standstillRefs = row.standstillRefs;
            std::snprintf(entry.context, sizeof(entry.context), "%s", row.ctx != nullptr ? row.ctx : "unknown");
            std::snprintf(entry.orderB, sizeof(entry.orderB), "%s", row.orderB != nullptr ? row.orderB : "unknown");
            entry.vramDeltaMb = row.vramDeltaMb;
            std::snprintf(entry.config, sizeof(entry.config), "%s", row.config != nullptr ? row.config : "");
            built.Add(entry);
        }
        return built;
    }();
    return table;
}

// The reference count a switch expects on the live chain: what the in-game probe measured when it ran, and otherwise the
// count the row measured at standstill. A row is only written with one value agreed by every run, so a build has one
// answer; the live count, when there is one, is the more specific question and wins.
inline uint32_t ExpectedRefs(const LibraryModel& row, uint32_t measured) { return measured != 0 ? measured : row.standstillRefs; }

// One line per row for the log and for the probe data:
//   [fg-probe] model class=fsrfg file=amd_fidelityfx_framegeneration_dx12.dll size=N stamp=65F1A2B3 config=direct
//              verdict=M2 witness=honoured refs=2
//   [fg-probe] model class=xefg file=libxess_fg.dll size=N stamp=71EF3D4D config=xell verdict=M2 witness=honoured refs=2
//              companionFile=libxell.dll companionSize=N companionStamp=6A561284
// The witness is spelled `honoured` or `no`, the same two tokens the data file takes: a row line is copied into that file
// by hand, so the host and the parser must not have two spellings for the same answer. `refs=` is the standstill count,
// and the companion fields are present for the classes that have one.
inline void FormatRow(const ModelRow& row, const char* prefix, char (&out)[512])
{
    char id[160] {};
    IdentityText(row.library, id);
    if (row.cls == Class::XeFg && !row.companion.Empty())
    {
        std::snprintf(out, sizeof(out),
                      "%s model class=%s %s config=%s verdict=%s witness=%s order=%s refs=%u companionFile=%s "
                      "companionSize=%llu companionStamp=%08X",
                      prefix, ClassText(row.cls), id, row.config[0] != '\0' ? row.config : "none",
                      ModelText(row.model.model), row.model.witnessHonoured ? "honoured" : "no",
                      row.pinFirst ? "pinfirst" : "destroyfirst", row.standstillRefs, row.companion.file,
                      static_cast<unsigned long long>(row.companion.size), row.companion.stamp);
        return;
    }
    std::snprintf(out, sizeof(out), "%s model class=%s %s config=%s verdict=%s witness=%s order=%s refs=%u", prefix,
                  ClassText(row.cls), id, row.config[0] != '\0' ? row.config : "none", ModelText(row.model.model),
                  row.model.witnessHonoured ? "honoured" : "no", row.pinFirst ? "pinfirst" : "destroyfirst",
                  row.standstillRefs);
}
} // namespace ext::fg::outswitch
