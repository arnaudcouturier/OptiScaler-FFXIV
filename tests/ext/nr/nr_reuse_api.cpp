#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <nvapi.h>
#include <vector>
#include <map>
#include <tuple>
#include <mutex>
#include <stdexcept>
#include <cstdio>
#include <thread>
#include <future>
#include <chrono>
#include "ext/nr/native/DlssNrVitReuse.h"
#include "ext/nr/native/DlssNrVitCapture.h"
#include "ext/nr/native/DlssNrNative.h"
#define LOG_INFO(...) ((void)0)
struct ObservedFunction { NVDX_ObjectHandle module=nullptr;std::string name; };
struct EvaluationObservation { uint64_t launches=0,known=0,unknown=0;bool reset=false;unsigned trace=0; };
EvaluationObservation& Observation(){static thread_local EvaluationObservation value;return value;}
struct Session { bool pending=false; };
struct State {
    DlssNrVitReuse::Filter vit;
    DlssNrVitReuse::RoleRegistry<NVDX_ObjectHandle> vitRole;
    std::map<NVDX_ObjectHandle,ObservedFunction>observedFunctions;
    std::map<std::tuple<ID3D12GraphicsCommandList*,int>,Session>sessions;
    std::recursive_mutex mutex;uint64_t evaluations=0,extendedCalls=0;bool restartRequired=false;
    std::string status;bool allowUnverified=false,enabled=false;
    std::atomic<decltype(&NvAPI_D3D12_LaunchCuKernelChainEx)>launchEx{nullptr};
};
State& S(){static State value;return value;}
std::vector<NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX> captured;
const NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX* received=nullptr;
unsigned calls=0;
NvAPI_Status __cdecl Driver(ID3D12GraphicsCommandList*,const NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX*k,NvU32 n){
    auto worker=std::async(std::launch::async,[]{
        if(!S().mutex.try_lock())return false;
        S().mutex.unlock();return true;
    });
    if(!worker.get())throw std::runtime_error("driver called while NR lock held");
    ++calls;received=k;captured.assign(k,k+n);return NVAPI_OK;
}
#include "nr_reuse_api_production.inl"
void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
int main() try {
    auto&s=S();s.launchEx=&Driver;
    const auto start=reinterpret_cast<NVDX_ObjectHandle>(1),finish=reinterpret_cast<NVDX_ObjectHandle>(2),other=reinterpret_cast<NVDX_ObjectHandle>(3);
    const auto module=reinterpret_cast<NVDX_ObjectHandle>(4);
    auto*cmd=reinterpret_cast<ID3D12GraphicsCommandList*>(5);int feature=0;
    s.vitRole.Add(start,module,DlssNrVitReuse::Role::Start);s.vitRole.Add(finish,module,DlssNrVitReuse::Role::End);
    const auto unknown=reinterpret_cast<NVDX_ObjectHandle>(99);
    s.vitRole.Add(unknown,module,DlssNrVitReuse::Role::Start,false);
    check(s.vitRole.Lookup(unknown)==DlssNrVitReuse::Role::None,"unknown runtime accepted by default");
    check(s.vitRole.Lookup(unknown,true)==DlssNrVitReuse::Role::Start,"override did not recognize existing function");
    check(s.vitRole.Lookup(other,true)==DlssNrVitReuse::Role::None,"override guessed unknown kernel");
    check(s.vitRole.RemoveFunction(unknown),"unverified function teardown not tracked");
    int argument=23;void* pointers[]{&argument};
    NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX kernels[3]{};
    kernels[0].hFunction=start;kernels[1].hFunction=finish;kernels[2].hFunction=other;
    kernels[2].kernelParams=pointers;kernels[2].gridDim={4,3,2};kernels[2].blockDim={32,4,1};kernels[2].dynSharedMemBytes=77;
    // Startup must bypass the lock even when another thread owns it.
    { std::promise<void> locked,release;auto ready=release.get_future();
      std::thread owner([&]{std::lock_guard guard(s.mutex);locked.set_value();ready.wait();});
      locked.get_future().wait();
      // A separate forwarding stub does not probe the intentionally held lock.
      s.launchEx=+[](ID3D12GraphicsCommandList*,const NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX*,NvU32)->NvAPI_Status{return NVAPI_OK;};
      auto startup=std::async(std::launch::async,[&]{return LaunchEx(cmd,kernels,3);});
      bool nonblocking=startup.wait_for(std::chrono::seconds(1))==std::future_status::ready;
      release.set_value();owner.join();startup.get();s.launchEx=&Driver;
      check(nonblocking,"unrelated startup blocked on NR lock");
    }
    s.vit.Begin(&feature,false,2);Observation()={};
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && received==kernels && captured.size()==3,"warmup did not preserve original EX launch");
    check(s.vit.End() && s.vit.Computed()==1,"warmup not counted");
    s.vit.Begin(&feature,false,2);Observation()={};
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && captured.size()==1,"EX reuse did not filter the verified range");
    check(captured[0].kernelParams==pointers && captured[0].gridDim.x==4 && captured[0].gridDim.z==2 && captured[0].blockDim.y==4 && captured[0].dynSharedMemBytes==77,"EX launch metadata or pointer-array ABI changed");
    check(s.vit.End() && s.vit.Reused()==1,"EX reuse not counted");
    // Unrelated destruction must not invalidate the known model cache.
    if(s.vitRole.RemoveFunction(other))s.vit.Clear();
    check(!s.vitRole.RemoveModule(reinterpret_cast<NVDX_ObjectHandle>(99)),"unrelated module was classified as NR");
    check(s.vitRole.RemoveModule(module),"known module removal not detected");
    s.observedFunctions[start]={module,"cc_vit_1d_repack_2d_to_1d_fp8"};
    s.vit.Begin(&feature,false,2);Observation()={};
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && received==kernels && captured.size()==3,"unverified runtime was filtered");
    check(Observation().unknown==1 && Observation().known==0 && Observation().launches==3,"unverified launch was not diagnosed");s.vit.End();
    s.vitRole.Add(start,module,DlssNrVitReuse::Role::Start,false);
    s.vitRole.Add(finish,module,DlssNrVitReuse::Role::End,false);
    const auto computed=s.vit.Computed(),reused=s.vit.Reused();
    BeginEvaluate(&feature,false,2,false);
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && captured.size()==3,"default filtered alternate runtime");s.vit.End();
    BeginEvaluate(&feature,false,2,true);
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && captured.size()==3,"override did not warm up");s.vit.End();
    BeginEvaluate(&feature,false,2,true);
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && captured.size()==1,"override did not reuse alternate runtime");s.vit.End();
    check(s.vit.Computed()==computed+1 && s.vit.Reused()==reused+1,"override counters wrong");
    BeginEvaluate(&feature,false,2,false);
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && captured.size()==3,"disable did not restore verification");s.vit.End();
    BeginEvaluate(&feature,false,2,true);
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && captured.size()==3,"re-enable reused stale cache");s.vit.End();
    BeginEvaluate(&feature,true,2,true);
    check(LaunchEx(cmd,kernels,3)==NVAPI_OK && captured.size()==3,"override ignored reset");s.vit.End();
    // The same production template must filter the basic launch ABI too.
    NVAPI_CU_KERNEL_LAUNCH_PARAMS basic[3]{};
    for(int i=0;i<3;++i)basic[i].hFunction=kernels[i].hFunction;
    basic[2].pParams=&argument;basic[2].paramSize=sizeof(argument);
    BeginEvaluate(&feature,false,2,true);std::vector<NVAPI_CU_KERNEL_LAUNCH_PARAMS> kept;
    check(VitDrop(s,basic,3,kept) && kept.size()==1 && kept[0].pParams==&argument,"basic override lost retained arguments");s.vit.End();
    s.enabled=true;BeginEvaluate(&feature,false,2,true);
    check(!s.vit.Evaluating(),"override allowed hybrid reuse");s.enabled=false;
    // A full downstream evaluation must not erase pass 1's alternating cache.
    int downstream=0;
    s.vit.Clear();
    const auto beforeComputed=s.vit.Computed(),beforeReused=s.vit.Reused();
    for(unsigned frame=0;frame<6;++frame){
        BeginEvaluate(&feature,false,DlssNrNative::ReuseEveryForPass(0,2),true);
        check(LaunchEx(cmd,kernels,3)==NVAPI_OK && captured.size()==(frame%2 ? 1u : 3u),"first pass lost alternating reuse");s.vit.End();
        for(unsigned pass=1;pass<4;++pass){
            BeginEvaluate(&downstream,false,DlssNrNative::ReuseEveryForPass(pass,2),true);
            check(!s.vit.Evaluating(),"downstream pass entered reuse");
            check(LaunchEx(cmd,kernels,3)==NVAPI_OK && captured.size()==3,"downstream pass dropped kernels");
        }
    }
    check(s.vit.Computed()==beforeComputed+3 && s.vit.Reused()==beforeReused+3,"multipass reuse savings lost");
    check(DlssNrNative::ReuseEveryForPass(0,1)==1,"reuse off changed");
    s.vit.Clear();
    for(unsigned frame=0;frame<4;++frame){
        for(unsigned pass=0;pass<2;++pass){
            BeginEvaluate(pass ? &downstream : &feature,false,DlssNrNative::ReuseEveryForPass(pass,2,false),true);
            check(LaunchEx(cmd,kernels,3)==NVAPI_OK && captured.size()==(frame%2 ? 1u : 3u),"original all-pass reuse not restored");s.vit.End();
        }
    }
    for(unsigned pass=0;pass<30;++pass)
        check(DlssNrNative::ReuseEveryForPass(pass,1,false)==1,"original mode ignored reuse disabled");
    // VitCapture is log-only: on the production LaunchEx/VitDrop/BeginEvaluate path it records every launch as issued and never
    // changes what reaches the driver, with the override off or on, with VitEvery=2 or 1, and once its budget is spent.
    {
        auto& cap = DlssNrVitCapture::Instance();
        const auto count = [](const std::string& text, const std::string& needle) {
            size_t n = 0;
            for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + needle.size())) ++n;
            return n;
        };
        const uint64_t hs = reinterpret_cast<uintptr_t>(start), hf = reinterpret_cast<uintptr_t>(finish), ho = reinterpret_cast<uintptr_t>(other);
        const auto arm = [&] {
            cap.ResetForTest(); cap.SetWanted(true);
            cap.NoteFunction(hs, 4, "cc_vit_1d_repack_2d_to_1d_fp8");
            cap.NoteFunction(hf, 4, "cc_vit_1d_repack_1d_to_2d_fp8");
            cap.NoteFunction(ho, 4, "cc_something_else");
            cap.TakePending();
            s.vit.Clear(); s.restartRequired = false; s.enabled = false;
        };
        const auto finishEval = [&] { s.vit.End(); check(cap.EndEvaluation(true), "capture did not close the evaluation"); return cap.TakePending(); };
        s.vitRole.Add(start, module, DlssNrVitReuse::Role::Start, false);
        s.vitRole.Add(finish, module, DlssNrVitReuse::Role::End, false);

        // 1. override off, VitEvery=2, module unverified: nothing is dropped (as without capture) and everything is recorded
        arm();
        const auto computed1 = s.vit.Computed(), reused1 = s.vit.Reused();
        for (unsigned e = 1; e <= DlssNrVitCapture::kEvaluations; ++e) {
            BeginEvaluate(&feature, false, 2, false);
            check(LaunchEx(cmd, kernels, 3) == NVAPI_OK && received == kernels && captured.size() == 3, "capture changed an unverified launch");
            const auto text = finishEval();
            check(count(text, "LAUNCH eval=" + std::to_string(e) + " ") == 3, "capture missed launches of an unverified module");
            check(count(text, "kernel=cc_vit_1d_repack_2d_to_1d_fp8 vit=1 role=start abi=ex") == 1 && count(text, "kernel=cc_something_else vit=0 role=none abi=ex") == 1, "capture lost kernel names/roles");
            check(text.find("pmode=ptrs pbytes=0 trunc=0 kp=0x") != std::string::npos, "capture did not mark the EX pointer-array form");
        }
        check(cap.Done() && s.vit.Computed() == computed1 && s.vit.Reused() == reused1, "capture touched the reuse counters");

        // 2. override on, verified roles: the dropped launches are recorded too, because recording precedes the drop decision
        arm();
        s.vitRole.Add(start, module, DlssNrVitReuse::Role::Start, false);
        s.vitRole.Add(finish, module, DlssNrVitReuse::Role::End, false);
        BeginEvaluate(&feature, false, 2, true);
        check(LaunchEx(cmd, kernels, 3) == NVAPI_OK && captured.size() == 3, "capture changed the warm-up launch");
        check(count(finishEval(), "LAUNCH eval=1 ") == 3, "warm-up launches not recorded");
        BeginEvaluate(&feature, false, 2, true);
        check(LaunchEx(cmd, kernels, 3) == NVAPI_OK && captured.size() == 1, "capture prevented the override from dropping");
        {
            const auto text = finishEval();
            check(count(text, "LAUNCH eval=2 ") == 3 && text.find("END eval=2 launches=3 recorded=3") != std::string::npos, "dropped launches were not recorded before the decision");
        }

        // 3. VitEvery=1 (capture only): the evaluation is watched, nothing is dropped, no cache or counter is touched
        arm();
        s.vitRole.Add(start, module, DlssNrVitReuse::Role::Start, true);
        s.vitRole.Add(finish, module, DlssNrVitReuse::Role::End, true);
        const auto c0 = s.vit.Computed(), r0 = s.vit.Reused();
        for (unsigned e = 1; e <= DlssNrVitCapture::kEvaluations; ++e) {
            BeginEvaluate(&feature, false, 1, false);
            check(s.vit.Evaluating() && s.vit.ObserveOnly(), "capture-only evaluation was not watched");
            check(LaunchEx(cmd, kernels, 3) == NVAPI_OK && received == kernels && captured.size() == 3, "VitEvery=1 capture changed a launch");
            const auto text = finishEval();
            check(count(text, "LAUNCH eval=" + std::to_string(e) + " ") == 3 && text.find("every=1 override=0 observe_only=1") != std::string::npos, "VitEvery=1 launches not recorded");
        }
        check(s.vit.Computed() == c0 && s.vit.Reused() == r0, "watched evaluations moved the reuse counters");
        // budget spent: VitEvery=1 is back to the untouched fast path, and VitEvery=2 does not record
        BeginEvaluate(&feature, false, 1, false);
        check(!s.vit.Evaluating(), "spent capture still holds evaluations open");
        check(LaunchEx(cmd, kernels, 3) == NVAPI_OK && captured.size() == 3 && !cap.HasPending(), "spent capture still records");
        BeginEvaluate(&feature, false, 2, false);
        check(LaunchEx(cmd, kernels, 3) == NVAPI_OK && !cap.HasPending(), "spent capture records a reuse evaluation");
        s.vit.End();
        // watching a verified module left no cache behind: the first VitEvery=2 evaluation computes in full
        arm();
        s.vitRole.Add(start, module, DlssNrVitReuse::Role::Start, true);
        s.vitRole.Add(finish, module, DlssNrVitReuse::Role::End, true);
        BeginEvaluate(&feature, false, 1, false);
        LaunchEx(cmd, kernels, 3);
        finishEval();
        cap.SetWanted(false);
        BeginEvaluate(&feature, false, 2, false);
        check(LaunchEx(cmd, kernels, 3) == NVAPI_OK && captured.size() == 3, "watching an evaluation warmed the reuse cache");
        s.vit.End();

        // 4. capture off: VitEvery=1 is not an evaluation, nothing is recorded
        arm();
        cap.SetWanted(false);
        BeginEvaluate(&feature, false, 1, false);
        check(!s.vit.Evaluating() && !cap.Capturing(), "capture off still watches");
        LaunchEx(cmd, kernels, 3);
        check(!cap.HasPending(), "capture off recorded something");

        // 5. a pending hybrid restart is never entered through the capture-only path
        arm();
        s.restartRequired = true;
        BeginEvaluate(&feature, false, 1, false);
        check(!s.vit.Evaluating() && !cap.Capturing(), "capture entered an evaluation after a required restart");
        s.restartRequired = false;

        // 6. basic ABI: parameter bytes are recorded as-is and the retained arguments still reach the driver
        arm();
        NVAPI_CU_KERNEL_LAUNCH_PARAMS watched[3]{};
        for (int i = 0; i < 3; ++i) watched[i].hFunction = kernels[i].hFunction;
        watched[2].pParams = &argument; watched[2].paramSize = sizeof(argument); watched[2].gridDim = {7, 1, 1};
        BeginEvaluate(&feature, false, 1, false);
        std::vector<NVAPI_CU_KERNEL_LAUNCH_PARAMS> keptWatched;
        check(!VitDrop(s, watched, 3, keptWatched) && keptWatched.empty(), "watched basic launches were filtered");
        {
            const auto text = finishEval();
            check(text.find("kernel=cc_something_else vit=0 role=none abi=basic grid=7x1x1 block=0x0x0 shared=0 psize=4 pmode=blob pbytes=4 trunc=0 params=17000000") != std::string::npos, "basic ABI parameter bytes not recorded");
        }

        // 7. an EX launch that only carries the pointer array is recorded without reading it
        arm();
        NVAPI_CU_KERNEL_LAUNCH_PARAMS_EX bogus[1]{};
        bogus[0].hFunction = other; bogus[0].kernelParams = reinterpret_cast<void**>(static_cast<uintptr_t>(0x10));
        BeginEvaluate(&feature, false, 1, false);
        check(LaunchEx(cmd, bogus, 1) == NVAPI_OK && received == bogus, "bogus pointer array changed the launch");
        check(finishEval().find("pmode=ptrs pbytes=0 trunc=0 kp=0x10 params=-") != std::string::npos, "pointer-array form not marked");

        cap.ResetForTest();
        s.vit.Clear(); s.vitRole.RemoveFunction(start); s.vitRole.RemoveFunction(finish);
        s.vitRole.Add(start, module, DlssNrVitReuse::Role::Start); s.vitRole.Add(finish, module, DlssNrVitReuse::Role::End);
    }
    const auto previous=calls;s.sessions[{cmd,0}].pending=true;s.vit.Begin(&feature,false,2);
    check(LaunchEx(cmd,kernels,3)==NVAPI_ERROR && calls==previous && s.restartRequired,"pending hybrid pair allowed incompatible extended completion");
    puts("PASS: extracted EX wrapper preserves kernelParams/geometry, checks default verification, live experimental override and cache reset, diagnoses unknown modules, records launches for VitCapture without changing them and refuses a pending hybrid pair");
    return 0;
} catch(const std::exception&e){puts(e.what());return 1;}
