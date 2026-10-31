#pragma once
// The provider, factory and process-state adapters of the live output switch (the runtime side of SwitchOpsContract.h).
//
// Four jobs, each one a thing the pure core cannot do:
//   * Capture: read what a creation needs from the chain that is on the window NOW (descriptor, factory, window, queue),
//     before anything is detached or released. Reading it later would be a use after free on the chain a library's own
//     destroy may already have ended.
//   * StateSnapshot / RestoreState: a frame-generation library creates its inner swapchain through the hooked factory, and
//     that path overwrites process-global state the whole DX11->DX12 bridge reads (descriptor, real/wrapped chain, the
//     D3D12 device and queue, the interop api, the tearing and exclusive-fullscreen flags, the window size). The switch
//     snapshots it before the create and restores it after every attempt, a failed one included. The three globals it
//     never writes are currentFGSwapchain, currentFG and activeFgOutput: those are the switch's own decision.
//   * Create: one target's chain through the guard set boot uses (FG swapchain creation skipped for the plain chain, the
//     parent wrapping skipped, the real factory entry), FSR FG and XeFG through upstream's own CreateSwapChain with the
//     factory's FG swapchain creation skipped, as boot does: the library creates its real chain through the hooked
//     factory, and without the skip that inner create is taken for a game's new swapchain and runs the provider's create
//     a second time from inside the first (FSR FG nests a second frame-interpolation swapchain, XeFG fails a second
//     initialisation of the context it is initialising). Every chain is created on the bridge's queue the switch pinned
//     (QueueRules.h). A create is retry-idempotent for one target: the provider shell is created once and reused by the
//     retries.
//   * LoadersReady / RowFor: the preflight's two expensive questions, answered once per target per session (the library
//     loads, and the table has a row for the build that is loaded) and cached.
//
// The witness object lives here too: a private-data interface on the outer chain whose release is the teardown's
// cross-check. It is a diagnostic that can stop a release, never one that causes it (ChainLifetime.h).
//
// Windows and upstream includes live in the .cpp; the header only needs DXGI's own types.

#include "ext/fg/outswitch/LibraryModel.h"
#include "ext/fg/outswitch/SwitchTypes.h"

#include <dxgi.h>
#include <dxgi1_6.h>
#include <windows.h>

#include <cstdint>

namespace ext::fg::outswitch::provider
{
// Everything a creation needs, read from the live chain while it is still alive.
struct Captured
{
    void* chain = nullptr; // the live chain the capture came from (never touched again after the teardown)
    HWND hwnd = nullptr;
    DXGI_SWAP_CHAIN_DESC desc {};
    IDXGIFactory* factory = nullptr;    // one reference, released by ReleaseCapture
    void* wrappedFactory = nullptr;     // the factory's WrappedIDXGIFactory7 when it has one (one reference too)
    // The bridge's own (unwrapped) factory, from the hidden real chain's parent. The plain chain is created through
    // it: the wrapped factory's create hands back a wrapped chain, and the bridge presents the real one itself.
    IDXGIFactory* realFactory = nullptr; // one reference, released by ReleaseCapture
    // Borrowed: the queue the next chain is created on, the bridge's queue the switch pinned (QueueRules.h). The pin keeps
    // it alive across every teardown, which the global command queue does not: a library's create points that one at the
    // library's own queue, and the library frees it in its destroy.
    IUnknown* queue = nullptr;
    bool valid = false;
};

// The globals a library's own factory re-entry overwrites while it creates its inner chain (3.2 of the design's evidence,
// read back from hooks/DxgiFactory_Hooks.cpp). `currentFGSwapchain`, `currentFG` and `activeFgOutput` are NOT part of it.
struct StateSnapshot
{
    DXGI_SWAP_CHAIN_DESC desc {};
    IDXGISwapChain* real = nullptr;
    IDXGISwapChain* wrapped = nullptr;
    // The global that names the chain on the window. The library's own factory re-entry replaces it with its inner FG
    // chain (FGHooks::SetFGSwapchain writes it), and the bridge and its readers expect the bridge object here.
    IDXGISwapChain* current = nullptr;
    ID3D12Device* d3d12Device = nullptr;
    ID3D11Device* d3d11Device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    uint32_t interopApi = 0;
    bool allowTearing = false;
    uint32_t lastFlags = 0;
    bool exclusiveFullscreen = false;
    float screenWidth = 0;
    float screenHeight = 0;
    // The D3D11 interop's device, queue and list type (WithDx12): the library's factory path points the interop at the
    // library's own queue, and the upscaler's evaluate thread would go on submitting there after that library is gone.
    void* interopDevice = nullptr;
    void* interopQueue = nullptr;
    uint32_t interopListType = 0;
};

// The live chain's descriptor and factory, plus the process's D3D12 queue. False when any of them is missing: the switch
// then refuses before anything is released (NoCapture).
bool CaptureLive(void* chain, Captured& out);
// Fills out.realFactory from the bridge's hidden real chain. False when it cannot be read; the plain create then
// falls back to the captured factory (the pre-fix behaviour).
bool CaptureRealFactory(void* bridge, Captured& out);
// The bridge's hidden real chain and its cached descriptor: the capture a limp retry or a rollback uses when no FG chain is
// on the window any more.
bool CaptureBridge(void* bridge, Captured& out);
void ReleaseCapture(Captured& capture);
// The descriptor of the chain a forward capture read on the window, before anything was torn down, kept per bridge: the
// descriptor CaptureBridge (a probation or watchdog rollback, a limp) creates the plain chain from. The process's global
// descriptor cannot serve there: a frame-generation library's inner chain rewrites it on every present (the wrapped
// chain's present reads its own descriptor into it), and XeFG's inner chain carries the frame-latency waitable flag, so a
// plain chain made from it is refused by every later preflight.
void RecordChainDesc(void* bridge, const DXGI_SWAP_CHAIN_DESC& desc);

// QueueRules.h, rule 1: the switch's own reference on the bridge's copy queue, taken at the first capture for a bridge
// (before anything is torn down) and given back when the switch moves to another bridge. Returns the pinned queue, or null
// when the bridge has none.
void* PinBridgeQueue(void* bridge);
void UnpinBridgeQueue();
// Rule 2: sets out.queue to the queue the next chain is created on. False when there is none: the capture then fails and
// the switch refuses before anything is touched.
bool ChooseCreationQueue(void* bridge, Captured& out);

// What the last create did to the queues, for the switch's own log line: the pinned queue, the creation queue, the
// interop's queue before the create and the queue the library's factory path left in it (before the restore).
struct QueueTrace
{
    void* pinned = nullptr;
    void* creation = nullptr;
    void* interopBefore = nullptr;
    void* interopClobbered = nullptr;
};
QueueTrace LastQueueTrace();
// The interop's queue now, and the game queue a provider was created with (null for a plain chain).
void* InteropQueueNow();
void* ProviderQueue(void* provider);
// The queue a provider's library created its real chain on (its present and overlay work runs there), when the switch
// created that provider: the queue the library's factory path left in the interop. Null for any other provider (the boot
// one's is the bridge's own queue) and for a plain chain. Borrowed: the library frees it in its destroy.
void* LibraryQueue(void* provider);

StateSnapshot SnapshotState();
void RestoreState(const StateSnapshot& snapshot);

// The loader question, asked once per target per session and cached (never per frame, and never LoadStreamline or
// CheckForFGStatus: those mutate the configuration when they fail).
bool LoadersReady(Target target);

// The identity of the module the class's present slot lives in (file name, size, PE TimeDateStamp), from the mapped image.
bool IdentityFor(Class cls, LibraryIdentity& out);
// The measured row for a class, looked up by that identity (and, for XeFG, the libxell.dll companion). An unknown build
// answers an empty row, which the preflight refuses.
LibraryModel RowFor(Class cls);

struct Created
{
    void* chain = nullptr;
    void* provider = nullptr;
    uint32_t hr = 0;
    bool denied = false; // the window is still bound to another chain (E_ACCESSDENIED)
};

// One create attempt for one target. The provider shell is created once per target (retry-idempotent): a second call for
// the same target reuses it, which is what makes the retry loop free of leaked shells.
Created Create(Target target, const Captured& capture);
// The plain chain's presenter registration (as boot does after a plain create) and its inverse.
void NotePlainPresented(void* chain, HWND hwnd);

// Teardown primitives. `provider` is an IFGFeature_Dx12*, `hwnd` the window the chain was created for.
void DeactivateProvider(void* provider);
bool HasSwapchainContext(void* provider);
bool ReleaseSwapchain(void* provider, void* hwnd);
void ShutdownProvider(void* provider);
uint32_t ReleasePin(void* chain);
bool SetBridgeChain(void* bridge, void* chain, void* provider);

// The private-data witness: placed before the destroy, released with the chain, read after it. One per session (the switch
// owns one chain at a time).
bool PlaceWitness(void* chain);
void ClearWitness();
bool WitnessAlive();
// The read-only question the preflight asks: can the interface be placed on this chain and cleared again? Nothing is
// released and no reference count changes; the answer only says whether a row that leans on the witness can lean on it here.
bool WitnessPlaceable(void* chain);
} // namespace ext::fg::outswitch::provider
