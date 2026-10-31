#pragma once
// The D3D12 queues of a live switch: the queue a new chain is created on, the queue the D3D11 interop is left on after a
// frame-generation library's create, and the reference the switch holds of its own.
//
// Three queues meet in a switch:
//   bridge   the bridge's own copy queue. The interop copy into the FG chain's back buffer, a plain chain's overlay and
//            the bridge's GPU timers submit there. The bridge takes it from the D3D11 interop when it is constructed,
//            which is after the boot library's create, so at a frame-generation boot it is that library's own queue.
//   interop  the D3D11 interop's queue (WithDx12). The upscaler's evaluate thread, the D3D11->D3D12 sync objects and
//            every interop path read it, on their own threads.
//   state    the process's global command queue. It follows the interop queue whenever the interop is asked for its
//            objects.
// A frame-generation library creates its real chain on a queue of its own, through the hooked factory, and that factory
// path points the interop and the global queue at it. The library releases that queue in its destroy, and nothing else
// holds it: measured on both libraries by the probe host (tools/fg-probe, --cycle: the queue's references are gone after
// the destroy). So a chain created on it, or an interop left on it, uses freed memory as soon as that library is torn
// down: the device is removed, or the driver faults.
//
// The rules that keep every queue the switch uses alive across any teardown:
//   1. the switch takes one reference of its own on the bridge's queue at its first capture, before anything is torn
//      down (the boot library still owns it then), and keeps it for the bridge's life;
//   2. every chain it creates, the plain chain and a provider's game queue alike, is created on that queue: the queue the
//      bridge's interop copy and a plain chain's overlay already submit to;
//   3. after a library's create the interop is given back the queue it had before the create, so no other thread is
//      ever left on the new library's own queue.
//
// Shared by the product (ProviderOps.cpp), the switch simulation (tests/ext/fg/fg_switch_com_sim.cpp) and the probe
// host's replay of the switch against the real libraries (tools/fg-probe/FgProbe.cpp, --replay), so the three cannot
// disagree about which queue a chain lands on.
//
// Pure: no Windows or upstream include. Queues are opaque pointers here.

namespace ext::fg::outswitch::queues
{
// Rule 1: the switch holds its own reference on the bridge's queue before it tears anything down.
inline constexpr bool kPinBridgeQueue = true;

// Rule 1, when: at a capture, whether the pin is (re)taken on `bridgeQueue`, the bridge's copy queue now, given the queue
// `pinned` the switch holds. The pin names a queue, not a bridge: a bridge the game recreated, even at the freed one's
// address, has the new boot library's queue as its copy queue, and that queue must be pinned before the first switch
// away from that library frees it.
inline bool RepinNeeded(void* pinned, void* bridgeQueue)
{
    return bridgeQueue != nullptr && pinned != bridgeQueue;
}

// Rule 2: the queue a new chain (the plain chain, or the provider's game queue) is created on. `pinned` is the bridge's
// queue the switch holds; `stateQueue` is the process's global command queue at the capture, which a library's create
// may have pointed at that library's own queue. Null when nothing is pinned: the capture then fails and the switch
// refuses before anything is touched.
inline void* CreationQueue(void* pinned, void* stateQueue)
{
    (void) stateQueue;
    return pinned;
}

// Rule 3: the interop queue once a library's create returned. `before` is the queue it had before the create,
// `clobbered` the queue the library's factory path left in it.
inline void* InteropAfterCreate(void* before, void* clobbered)
{
    return before != nullptr ? before : clobbered;
}
} // namespace ext::fg::outswitch::queues
