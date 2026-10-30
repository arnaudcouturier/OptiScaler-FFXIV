// Source-text pins for the wiring of the Foveated profile and the settle in the NR pass and its shaders. They cannot prove the
// behaviour (the pure pieces have their own tests, the shaders run on WARP in nr_spatial_warp_smoke and nr_community_shader);
// they keep the structure that makes it true: one place chooses the geometry every consumer reads, the park follows the change,
// each scratch resource has its own allocation check, both shaders branch on the profile.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
int failures = 0;
void Check(bool ok, const char* what, int line)
{
    if (!ok)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++failures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

std::string Squash(const std::string& text)
{
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            out += c;
    return out;
}

std::string Read(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return Squash(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
}

size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++n;
    return n;
}

bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
} // namespace

int main(int argc, char** argv)
{
    const std::string repo = argc > 1 ? argv[1] : ".";
    const std::string nr = repo + "/OptiScaler/ext/nr/";
    const std::string pass = Read(nr + "pass/DlssNr_Dx12.cpp");
    const std::string common = Read(nr + "pass/DlssNr_Common.h");
    const std::string spatialShader = Read(nr + "shaders/dlssnr_spatial.hlsl");
    const std::string mainShader = Read(nr + "shaders/dlssnr.hlsl");
    const std::string warp = Read(nr + "shaders/dlssnr_spatial_warp.hlsli");
    const std::string menu = Read(nr + "menu/DlssNr_Menu.cpp");
    CHECK(pass.size() > 10000 && mainShader.size() > 10000 && spatialShader.size() > 1000 && warp.size() > 500 && menu.size() > 10000);

    // ---- one place chooses what every consumer reads ------------------------------------------------------------------------
    CHECK(Count(pass, "DlssNr::Settle::Pick(") == 1);
    CHECK(Count(pass, "DlssNr::Settle::Decide(") == 1);
    CHECK(!Has(pass, "DlssNr::Spatial::Build(")); // the pass asks for geometry, never a bare layout
    CHECK(Count(pass, "DlssNr::Spatial::MakeGeometry(") == 2);  // the request, and again after a spatial fallback is cleared
    const size_t pick = pass.find("DlssNr::Settle::Pick(");
    CHECK(pick != std::string::npos);
    if (pick != std::string::npos)
    {
        const std::string after = pass.substr(pick);
        // after the choice only the geometry is read: the request appears once more, to be adopted
        CHECK(Count(after, "requested.") == 0);
        CHECK(Count(after, "=requested;") == 1);
        CHECK(Has(after, "autospatialLayout=geometry.layout;"));
        CHECK(Has(after, "constfloatworkScale=geometry.workScale;"));
        CHECK(Has(after, "constautoworkWidth=geometry.workW;") && Has(after, "constautoworkHeight=geometry.workH;"));
        CHECK(Has(after, "constautoordinaryWidth=geometry.ordinaryW;") && Has(after, "constautoordinaryHeight=geometry.ordinaryH;"));
        CHECK(Has(after, "g_modelDimensions.store((uint64_t(workWidth)<<32)|workHeight,"));
        CHECK(Has(after, "StatusText(geometry,g_nr.spatialStatus") || Has(after, "StatusText(geometry,g_nr.spatialFailed,fusedSpatial)"));
        CHECK(Has(after, "FusedNative(geometry,"));
        // the adoption comes after every read of the request and only for the actions that adopt
        CHECK(Has(after, "if(DlssNr::Settle::AdoptsRequested(settleAction))g_nr.applied=requested;"));
    }
    // a frame size, placement, tuning or fallback change never waits
    CHECK(Has(pass, "constboolimmediate=!consistent||g_nr.spatialFailed||anamorphicRefused||placementChanged||tuningChanged;"));
    // ... and so does the replacement of a model size the runtime refused (the running geometry is the refused, per-axis one)
    CHECK(Count(pass, "constboolanamorphicRefused=g_nr.anamorphic.failed&&g_nr.applied.Anamorphic();") == 1);
    // the old per-frame overwrite and the unused frame counter are gone
    CHECK(!Has(pass, "g_nr.spatialLayout"));
    CHECK(Count(pass, "kSettleFrames") <= 1);
    // a warp-only change resets the history and rebuilds nothing
    CHECK(Has(pass, "if(settleAction==DlssNr::Settle::Action::HistoryReset){") && Has(pass, "g_nr.reset=true;for(auto&pendingReset:g_nr.passNeedsReset)pendingReset=true;"));
    CHECK(Has(pass, "constboolresolutionChanged=frameChanged||workChanged;"));
    CHECK(!Has(pass, "spatialChanged"));

    // ---- the park follows what the change invalidates ------------------------------------------------------------------------
    CHECK(Count(pass, "ParkNrResource(g_nr.colorCopy);") == 1 && Count(pass, "ParkNrResource(g_nr.hdrCopy);") == 1);
    CHECK(Count(pass, "ParkNrResource(g_nr.activeColor);") == 1 && Count(pass, "ParkNrResource(g_nr.output);") == 1);
    const size_t frameBlock = pass.find("if(DlssNr::Settle::ParksFrameScratch(parkScope)){");
    const size_t workBlock = pass.find("if(DlssNr::Settle::ParksWorkScratch(parkScope))ParkWorkSizeScratch();");
    CHECK(frameBlock != std::string::npos && workBlock != std::string::npos && workBlock < frameBlock);
    if (frameBlock != std::string::npos && workBlock != std::string::npos)
    {
        const std::string work = pass.substr(workBlock, frameBlock - workBlock);
        const std::string frameScratch = pass.substr(frameBlock, 300);
        CHECK(!Has(work, "g_nr.colorCopy") && !Has(work, "g_nr.hdrCopy") && !Has(work, "g_nr.activeColor"));
        CHECK(Has(frameScratch, "ParkNrResource(g_nr.colorCopy);") && Has(frameScratch, "ParkNrResource(g_nr.hdrCopy);"));
        CHECK(Has(frameScratch, "ParkNrResource(g_nr.activeColor);") && Has(frameScratch, "ParkNrResource(g_nr.outputNative);"));
    }
    // what is sized by the model's working size is parked by ONE function, used by the change of size and by the replacement of a refused size
    {
        const size_t helper = pass.find("voidParkWorkSizeScratch(){");
        const size_t helperEnd = pass.find("voidTickNrRetired()", helper == std::string::npos ? 0 : helper);
        CHECK(helper != std::string::npos && helperEnd != std::string::npos && helper < helperEnd);
        if (helper != std::string::npos && helperEnd != std::string::npos && helper < helperEnd)
        {
            const std::string body = pass.substr(helper, helperEnd - helper);
            CHECK(Has(body, "ParkNrResource(g_nr.output);ParkNrResource(g_nr.passScratch);ParkNrResource(g_nr.feedbackScratch);g_nr.feedbackScratchFailed=false;"));
            CHECK(Has(body, "ParkNrResource(g_nr.colorSmall);ParkNrResource(g_nr.depthSmall);ParkNrResource(g_nr.motionSmall);"));
            CHECK(Has(body, "&g_nr.spatialColor,&g_nr.spatialDepth,&g_nr.spatialMotion,&g_nr.spatialProxy,&g_nr.spatialAnswer"));
            CHECK(Has(body, "g_nr.passScratchFailed=false;"));
            CHECK(!Has(body, "g_nr.colorCopy") && !Has(body, "g_nr.hdrCopy") && !Has(body, "g_nr.activeColor") && !Has(body, "g_nr.outputNative"));
        }
        CHECK(Count(pass, "ParkWorkSizeScratch();") == 2); // the change of size, and the create of a refused size
    }
    // each scratch resource is allocated on its own check (a kept copy is never overwritten, a parked one is made again)
    CHECK(Has(pass, "if(g_nr.output==nullptr){g_nr.output=CreateScratch(device,desc.Format,workWidth,workHeight);g_nr.workWidth=workWidth;g_nr.workHeight=workHeight;}"));
    CHECK(Has(pass, "if(g_nr.colorCopy==nullptr)g_nr.colorCopy=CreateScratch(device,desc.Format,width,height);"));
    CHECK(Has(pass, "if(g_nr.hdrCopy==nullptr)g_nr.hdrCopy=CreateScratch(device,desc.Format,width,height);"));

    // ---- constants of both profiles reach the dispatches ----------------------------------------------------------------------
    CHECK(Has(pass, "DlssNr::Spatial::MakeResolveWarp(spatialLayout,fusedSpatial)"));
    CHECK(Has(pass, "resolveParams.SpatialResidual=resolveWarp.residual;"));
    CHECK(Count(pass, "DlssNr::Spatial::MakeConstants(spatialLayout,") == 3);
    CHECK(Has(common, "uint32_tSpatialResidual;") && Has(common, "static_assert(sizeof(DlssNrConstants)==256);"));

    // ---- both shaders branch on the profile; the unpack and resolve read the selector word's low byte ---------------------------
    CHECK(Has(warp, "#ifndefPW_FOVEATED") && Has(warp, "#errorPW_FOVEATED") && Has(warp, "if(PW_FOVEATED)"));
    CHECK(Count(warp, "if(PW_FOVEATED)") == 2); // pack and unpack
    CHECK(Has(warp, "FvPackedDistance(") && Has(warp, "FvUnpackedDistance(") && Has(warp, "[unroll]for(inti=0;i<4;++i)"));
    CHECK(Has(spatialShader, "#definePW_FOVEATED((gOptions.z&2u)!=0u)"));
    CHECK(spatialShader.find("#definePW_FOVEATED") < spatialShader.find("#include\"dlssnr_spatial_warp.hlsli\""));
    CHECK(Has(mainShader, "#definePW_FOVEATED((gSpatialResidual&256u)!=0u)") && Has(mainShader, "#definegSpatialMode(gSpatialResidual&255u)"));
    CHECK(mainShader.find("#definePW_FOVEATED") < mainShader.find("#include\"dlssnr_spatial_warp.hlsli\""));
    CHECK(Has(mainShader, "gSpatialMode==2?PwPack(") && Has(mainShader, "(modelRanSmall||gSpatialMode!=0)"));
    CHECK(!Has(mainShader, "gSpatialResidual==2") && !Has(mainShader, "gSpatialResidual!=0"));
    CHECK(Has(mainShader, "gSpatialPadding==1")); // the HDR carrier selector keeps its field

    // ---- the decision inputs and what follows from them (each fragment appears exactly once) -------------------------------------
    // The history reset must sit inside its own branch: the same statements appear in other places of the pass.
    const size_t resetBranch = pass.find("if(settleAction==DlssNr::Settle::Action::HistoryReset){");
    const size_t resetLog = pass.find("LOG_INFO(\"DLSS-NR:spatiallayoutchanged,modelhistoryreset(norebuild)\");");
    CHECK(Count(pass, "if(settleAction==DlssNr::Settle::Action::HistoryReset){") == 1 && Count(pass, "modelhistoryreset(norebuild)") == 1);
    CHECK(resetBranch != std::string::npos && resetLog != std::string::npos && resetBranch < resetLog);
    if (resetBranch != std::string::npos && resetLog != std::string::npos && resetBranch < resetLog)
        CHECK(Has(pass.substr(resetBranch, resetLog - resetBranch), "g_nr.reset=true;for(auto&pendingReset:g_nr.passNeedsReset)pendingReset=true;"));
    // the identity of what runs is what the running model reports, not the geometry's own record
    CHECK(Count(pass, "appliedId.frameW=g_nr.width;") == 1 && Count(pass, "appliedId.frameH=g_nr.height;") == 1);
    CHECK(Count(pass, "appliedId.workW=g_nr.workWidth;") == 1 && Count(pass, "appliedId.workH=g_nr.workHeight;") == 1);
    // ... and the identity adopted is the new frame and the new working size
    CHECK(Count(pass, "adoptedId.frameW=width;") == 1 && Count(pass, "adoptedId.frameH=height;") == 1);
    CHECK(Count(pass, "adoptedId.workW=workWidth;") == 1 && Count(pass, "adoptedId.workH=workHeight;") == 1);
    // a geometry that no longer matches the scratch is replaced at once
    CHECK(Count(pass, "constboolconsistent=g_nr.applied.workW==g_nr.workWidth&&g_nr.applied.workH==g_nr.workHeight;") == 1);
    // the settle runs on a real clock in milliseconds and gets the flag that ends the waiting
    CHECK(Count(pass, "constautonowMs=(int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();") == 1);

    // The output size the geometry is built for is the declared output (`outputW`/`outputH`: the Out parameters, else the
    // colour allocation), never the target allocation (`desc`): the Foveated pixel inputs are output pixels and the pass converts
    // them to the model's frame (pre-SR: the frame is smaller). Scratches and dispatches address the ACTIVE rectangle (the declared
    // size clamped to the allocation per axis); geometry and readout keep the declared size.
    CHECK(Count(pass, "MakeGeometry(requestedSettings,frame.BeforeUpscale?width:outputW,frame.BeforeUpscale?height:outputH,outputW,outputH,scaleX,scaleY,g_nr.spatialFailed);") == 1);
    CHECK(Count(pass, "MakeGeometry(requestedSettings,frame.BeforeUpscale?width:outputW,frame.BeforeUpscale?height:outputH,outputW,outputH,scaleX,scaleY,false);") == 1);
    // the scale of each axis is the per-axis key when set, otherwise the uniform one: X reads the X key, Y reads the Y key; one function
    // (shared with the menu) applies the refusal latch and the mixed above/below 100% rule to them
    CHECK(Count(pass, "constfloatscaleKeyX=nr.workingScaleX.value_or_default();") == 1);
    CHECK(Count(pass, "constfloatscaleKeyY=nr.workingScaleY.value_or_default();") == 1);
    CHECK(Count(pass, "constfloatscaleKeyUniform=nr.workingScale.value_or_default();") == 1);
    CHECK(Count(pass, "g_nr.anamorphic=DlssNr::RefreshAnamorphicLatch(g_nr.anamorphic,scaleKeyX,scaleKeyY,scaleKeyUniform);") == 1);
    CHECK(Count(pass, "constDlssNr::AxisScalesaxisScales=DlssNr::EffectiveScales(g_nr.anamorphic,scaleKeyX,scaleKeyY,scaleKeyUniform);") == 1);
    // not const: the frame-rate governor's run-time cap lowers them right after this rule
    CHECK(Count(pass, "floatscaleX=axisScales.x;") == 1 && Count(pass, "floatscaleY=axisScales.y;") == 1);
    CHECK(Count(pass, "scaleX=std::min(scaleX,g_nr.governorOut.cap);") == 1 && Count(pass, "scaleY=std::min(scaleY,g_nr.governorOut.cap);") == 1);
    CHECK(!Has(pass, "DlssNr::EffectiveAxisScale(")); // nothing in the pass computes a scale by itself any more
    // the order: the latch is refreshed against the keys first, then the scales are taken, then the geometry is built from them
    {
        const size_t refresh = pass.find("g_nr.anamorphic=DlssNr::RefreshAnamorphicLatch(");
        const size_t scales = pass.find("constDlssNr::AxisScalesaxisScales=DlssNr::EffectiveScales(");
        const size_t geometryCall = pass.find("DlssNr::Spatial::MakeGeometry(requestedSettings");
        CHECK(refresh != std::string::npos && scales != std::string::npos && geometryCall != std::string::npos && refresh < scales && scales < geometryCall);
    }
    // a model size the runtime refuses fails open: the create (before the session-ending failure) and the evaluate latch it, the keys that
    // asked for it are remembered, and a runtime switch forgets it; the session-ending failure stays for everything that is not anamorphic
    CHECK(Count(pass, "g_nr.anamorphic=DlssNr::LatchAnamorphic(scaleKeyX,scaleKeyY,scaleKeyUniform);") == 2);
    CHECK(Count(pass, "g_nr.anamorphic={};") == 1);
    {
        const size_t createBranch = pass.find("if(geometry.Anamorphic()){g_nr.anamorphic=DlssNr::LatchAnamorphic(scaleKeyX,scaleKeyY,scaleKeyUniform);ParkWorkSizeScratch();");
        const size_t createFail = pass.find("ext::nr::runtime::FailModel(g_nr.fail,createReason);");
        CHECK(createBranch != std::string::npos && createFail != std::string::npos && createBranch < createFail);
        CHECK(Count(pass, "ext::nr::runtime::FailModel(g_nr.fail,createReason);") == 1 && Count(pass, "ext::nr::runtime::FailModel(g_nr.fail,evaluateReason);") == 1);
        if (createBranch != std::string::npos && createFail != std::string::npos && createBranch < createFail)
            CHECK(Has(pass.substr(createBranch, createFail - createBranch), "device->Release();return;}")); // the refused create returns for a fresh try next frame
    }
    CHECK(Count(pass, "constboolanamorphicFallback=!spatial&&geometry.Anamorphic();") == 1);
    CHECK(Count(pass, "if(spatial){g_nr.spatialFailed=true;g_nr.spatialStatus=\"fallback:packedNRevaluationfailed\";}elseif(anamorphicFallback)g_nr.anamorphic=DlssNr::LatchAnamorphic(scaleKeyX,scaleKeyY,scaleKeyUniform);elseext::nr::runtime::FailModel(g_nr.fail,evaluateReason);") == 1);
    CHECK(Count(pass, "AnamorphicLatchAnamorphicLatchState(){std::lock_guard<std::recursive_mutex>lock(g_nrMutex);returng_nr.anamorphic;}") == 1);
    CHECK(Count(pass, "DlssNr::AnamorphicLatchanamorphic{};") == 1);
    // the guides and the motion vectors follow the model's size on each axis
    CHECK(Count(pass, "DlssNr::MatchGuidesToWork(spatial,workWidth,workHeight,width,height,geometry.Anamorphic())") == 1);
    CHECK(Count(pass, "DlssNr::ModelMotionScales(matchedGuides?workWidth:motionWidth,matchedGuides?workHeight:motionHeight,refW,refH);") == 1);
    CHECK(Count(pass, "constfloatmvToWorkX=mvToWork.x;") == 1 && Count(pass, "constfloatmvToWorkY=mvToWork.y;") == 1);
    // the reduced-model guide textures and the small colour raster are sized by the model's own width and height
    CHECK(Count(pass, "g_nr.depthSmall=CreateScratch(device,DXGI_FORMAT_R32_FLOAT,workWidth,workHeight);") == 1);
    CHECK(Count(pass, "g_nr.motionSmall=CreateScratch(device,DXGI_FORMAT_R32G32_FLOAT,workWidth,workHeight);") == 1);
    CHECK(Count(pass, "g_nr.colorSmall=CreateScratch(device,desc.Format,workWidth,workHeight);") == 1);
    CHECK(Count(pass, "resize.Width=workWidth;resize.Height=workHeight;") == 1);
    // the model is evaluated at its own width and height
    CHECK(Count(pass, "passInput,depthIn,motionIn,passOutput,workWidth,workHeight,") == 1);
    // and the menu readout reports that same output size, width then height
    CHECK(Count(pass, "requested.workW,requested.workH,outputW,outputH};") == 1);
    CHECK(Count(pass, "settleAction=DlssNr::Settle::Decide(g_nr.settle,appliedId,DlssNr::Spatial::IdentityOf(requested),nowMs,immediate);") == 1);
    // a change of the placement parks everything; otherwise what the change invalidates
    CHECK(Count(pass, "constautoparkScope=placementChanged?DlssNr::Settle::ParkScope::All:DlssNr::Settle::ParkScopeFor(appliedId,adoptedId);") == 1);
    // a change of either side of the frame or of the working size counts
    CHECK(Count(pass, "constboolframeChanged=g_nr.feature!=nullptr&&(g_nr.width!=width||g_nr.height!=height);") == 1);
    // the staleness of the work-size scratch is decided by the pure helper, from the applied identity (whose ordinary grid comes from
    // the applied geometry) against the geometry about to run; a park scope that sees the ordinary grid needs it in the adopted one
    CHECK(Count(pass, "constboolworkChanged=g_nr.feature!=nullptr&&DlssNr::Settle::WorkScratchStale(appliedId,workWidth,workHeight,ordinaryWidth,ordinaryHeight);") == 1);
    CHECK(Count(pass, "appliedId=DlssNr::Spatial::IdentityOf(g_nr.applied);") == 1);
    CHECK(Count(pass, "adoptedId.ordW=ordinaryWidth;") == 1 && Count(pass, "adoptedId.ordH=ordinaryHeight;") == 1);
    // a new layout gets a fresh attempt after a spatial fallback; an unchanged one does not
    CHECK(Count(pass, "if(g_nr.spatialFailed&&requested.layout!=g_nr.applied.layout)") == 1);
    // the menu readout: whether the profile is asked for, whether it is in use, the share only while it is in use
    CHECK(Count(pass, "requested.layout.requested,requested.spatial,requested.spatial?requested.layout.pixelShare:1.0f,requested.workW,requested.workH,") == 1);

    // ---- the menu: profile combo, placement limits, which sliders each profile shows, the readout -----------------------------------
    CHECK(Count(menu, "staticconstchar*spatialProfiles[]={\"Peripheral\",\"Foveated\"};") == 1);
    CHECK(Count(menu, "intspatialProfile=nr.spatialProfile.value_or_default()==1?1:0;") == 1);
    CHECK(Count(menu, "nr.spatialProfile=(uint32_t)spatialProfile;") == 1);
    CHECK(Count(menu, "constfloatmaxOffsetX=Spatial::MaxCenterOffset(centreXPercent);") == 1);
    CHECK(Count(menu, "constfloatmaxOffsetY=Spatial::MaxCenterOffset(centreYPercent);") == 1);
    // each axis of the Foveated centre is a share of the same axis of the output
    CHECK(Count(menu, "Foveated::PercentOf(placement.foveaWidth,placementReadout.outputW!=0?placementReadout.outputW:3840u)") == 1);
    CHECK(Count(menu, "Foveated::PercentOf(placement.foveaHeight,placementReadout.outputH!=0?placementReadout.outputH:2160u)") == 1);
    // the work shift belongs to the Peripheral profile only
    CHECK(Count(menu, "if(!foveated){DeferredSlider(\"Workshift(horizontal)\",&nr.spatialShiftX,shiftLimitsX.first,shiftLimitsX.second,0.0f,\"%.1f%%\");") == 1);
    CHECK(Count(menu, "DeferredSlider(\"Workshift(vertical)\",&nr.spatialShiftY,shiftLimitsY.first,shiftLimitsY.second,0.0f,\"%.1f%%\");") == 1);
    // the readout prints the share, then the model width and height in that order
    CHECK(Count(menu, "ImGui::TextWrapped(\"NRpixels:%.0f%%offull(model%ux%u)\",readout.share*100.0f,readout.modelW,readout.modelH);") == 1);

    // the NR time readout: next to the pixel share (spatial block) and under the model size; the read of the GPU time is under the pass lock
    CHECK(Count(menu, "ImGui::TextDisabled(\"%s\",NrTimeLine().c_str());") == 2);
    CHECK(Count(menu, "staticstd::stringNrTimeLine(){returnNrTimeText(IsRunningVk()?LastGpuTimeVk():LastGpuTime());}") == 1);
    CHECK(Count(menu, "readout.modelW,readout.modelH);ImGui::TextDisabled(\"%s\",NrTimeLine().c_str());") == 1);
    CHECK(Count(menu, "(modelHeight+1)/2);ImGui::TextDisabled(\"%s\",NrTimeLine().c_str());") == 1);
    CHECK(Count(pass, "std::optional<double>LastGpuTime(){std::lock_guard<std::recursive_mutex>lock(g_nrMutex);returng_lastGpuTime;}") == 1);

    // ---- depth-aware enlargement (key DepthAwareResolve) ---------------------------------------------------------------------------------
    {
        const std::string header = Read(nr + "pass/DlssNr_Dx12.h");
        const std::string slots = Read(nr + "pass/DlssNr_DescriptorSlots.h");
        // One more input in the descriptor table of every dispatch, none more dispatches: the reservations count dispatches (one
        // slot, and so one descriptor heap, per DispatchPass), so the budget and the pool stay as they were.
        CHECK(Count(header, "staticconstexpruint32_tkSrvCount=6;") == 1 && Count(header, "staticconstexpruint32_tkUavCount=2;") == 1);
        CHECK(Count(header, "#defineDLSSNR_NUM_OF_HEAPS384") == 1);
        CHECK(Count(pass, "_descriptorSlots.HasCapacity(8+2*requestedPasses)") == 1);
        CHECK(Count(pass, "_descriptorSlots.Acquire(") == 1);
        CHECK(Count(header, "ID3D12Resource*InDepth=nullptr);") == 1);
        CHECK(Count(pass, "ID3D12PipelineState*pipeline,ID3D12Resource*InDepth)") == 1);
        CHECK(Count(pass, "InPrevEdit!=nullptr?InPrevEdit:InSource,InDepth!=nullptr?InDepth:InSource,};") == 1); // the stand-in, like every spare input
        CHECK(Has(slots, "template<unsignedCapacity>classDescriptorSlots"));
        // the game's depth is taken right after it is made readable, before the guides are replaced by the working-size copies
        const size_t made = pass.find("ID3D12Resource*depthIn=ReadableGuide(device,cmdList,depth,&g_nr.depthClone);");
        const size_t captured = pass.find("ID3D12Resource*constnativeDepth=depthIn;");
        const size_t reassigned = pass.find("depthIn=g_nr.spatialDepth;");
        CHECK(made != std::string::npos && captured != std::string::npos && reassigned != std::string::npos && made < captured && captured < reassigned);
        CHECK(Count(pass, "ID3D12Resource*constnativeDepth=depthIn;") == 1);
        // the decision and what the resolve is given; the spatial warp takes precedence and the bit is the only thing set
        CHECK(Count(pass, "DlssNr::DepthAware::Decide(nr.depthAwareResolve.value_or_default(),spatial,DlssNr::DepthAware::Enlarging(workWidth,workHeight,width,height,workScale>1.0f),nativeDepth!=nullptr,workScale>1.0f);") == 1);
        CHECK(Count(pass, "resolveParams.SpatialResidual=DlssNr::DepthAware::kResolveBit;") == 1);
        // the depth is handed to the resolve only when the decision is Active, together with the selector bit and its parameters
        CHECK(Count(pass, "resolveParams.SpatialResidual=DlssNr::DepthAware::kResolveBit;DlssNr::DepthAware::PackFields(resolveParams.SpatialWarp,depthRect,g_nr.guideDepthInverted);resolveDepth=nativeDepth;") == 1);
        CHECK(Count(pass, "if(depthState==DlssNr::DepthAware::State::Active){") == 1);
        // a dispatch without a depth cannot carry the selector bit (the sixth input is then the source as a stand-in); only the main shader
        // reads that word, the spatial and guide shaders keep their own constants in the same bytes
        CHECK(Count(pass, "DlssNrConstantsconstants=InConstants;if(pipeline==nullptr)constants.SpatialResidual=DlssNr::DepthAware::ResolveSelector(constants.SpatialResidual,InDepth!=nullptr);") == 1);
        CHECK(Count(pass, "InConstants") == 2 && Count(pass, "CreateConstantsBuffer(_device,_constantBuffers[slot],constants,currentHeap.GetCbvCPU(0))") == 1);
        CHECK(Count(pass, "constUINTdispatchWidth=(constants.Width+_numThreadsX-1)/_numThreadsX;") == 1 && Count(pass, "constUINTdispatchHeight=(constants.Height+_numThreadsY-1)/_numThreadsY;") == 1);
        // the status goes where the spatial status is reset, so a frame that returns early does not keep an old answer
        CHECK(Count(pass, "g_nr.spatialStatus=DlssNr::Spatial::StatusText(geometry,g_nr.spatialFailed,fusedSpatial);g_nr.depthAwareStatus.clear();") == 1);
        // the weight sum under which the plain filter is used comes from the same constant as the host reference (packed in a slot)
        CHECK(Count(mainShader, "constbooldepthWeighted=usable&&sum>gDepthMinSum;") == 1 && Count(mainShader, "#definegDepthMinSumgSideNeg.w") == 1);
        CHECK(!Has(mainShader, "sum>1e-4") && !Has(mainShader, "sum>0.0001") && !Has(mainShader, "usable&&sum>1"));
        // Vulkan reads the GPU time under the lock the evaluation writes it with
        CHECK(Count(Read(nr + "vk/DlssNrFeature_Vk.cpp"), "std::optional<double>LastGpuTimeVk(){std::lock_guard<std::mutex>time(g_vkTimeMutex);returng_vk.lastGpuTime;}") == 1);
        // the motion vectors handed to the model follow the model's size on each axis (the X ratio on X, the Y ratio on Y)
        CHECK(Count(pass, "spatial?1.0f:g_nr.guideMvScaleX*mvToWorkX,") == 1 && Count(pass, "spatial?1.0f:g_nr.guideMvScaleY*mvToWorkY);") == 1);
        CHECK(Count(pass, "DlssNr::DepthAware::PackFields(resolveParams.SpatialWarp,depthRect,g_nr.guideDepthInverted);") == 1);
        CHECK(Count(pass, "constfloatdepthRect[4]={(float)depthBaseX,(float)depthBaseY,(float)guideWidth,(float)guideHeight};") == 1);
        CHECK(Count(pass, "resolveTarget,nullptr,nullptr,resolveDepth);") == 1);
        CHECK(Count(pass, "std::stringDepthAwareStatus(){std::lock_guard<std::recursive_mutex>lock(g_nrMutex);returng_nr.depthAwareStatus;}") == 1);
        // the shader: the input exists on Direct3D 12 only, the feature needs the spatial mode byte at 0, the plain samples stay in the other branch
        CHECK(Count(mainShader, "#ifndefVK_MODETexture2D<float4>gDepth:register(t5);#endif") == 1);
        CHECK(Count(mainShader, "#defineDEPTH_AWARE(((gSpatialResidual&512u)!=0u)&&(gSpatialResidual&255u)==0u)") == 1);
        CHECK(Count(mainShader, "[branch]if(DEPTH_AWARE&&ModelIsReduced())DepthAwareSamples(cmpUv,proxySample,modelSample);else#endif{proxySample=gSource.SampleLevel(gLinear,modelUv,0);modelSample=gModel.SampleLevel(gLinear,modelUv,0);}") == 1);
        CHECK(Count(mainShader, "intlast=int2(tw,th)-1;") == 0 && Count(mainShader, "constint2last=int2(tw,th)-1;") == 1);
        CHECK(Has(mainShader, "int2texel=clamp(int2(gDepthRect.xy+frameUv*gDepthRect.zw),lo,hi);") && Has(mainShader, "gDepth.Load(int3(texel,0)).x"));
        CHECK(Has(mainShader, "#definegDepthRectgBandCenter") && Has(mainShader, "#definegDepthSigmagSideNeg.x") && Has(mainShader, "#definegDepthEpsilongSideNeg.y"));
        CHECK(Has(mainShader, "#definegDepthReversed(gSideNeg.z!=0.0)"));
        CHECK(Has(mainShader, "floatDepthDisparity(floatd){returngDepthReversed?d:1.0-d;}"));
        CHECK(Has(mainShader, "exp(-abs(pixelDisparity-DepthDisparity(tapDepth))/(gDepthSigma*max(pixelDisparity,gDepthEpsilon)))"));
        // the menu: the checkbox is greyed with spatial compression and below a reduced model, and its status line follows the pass
        CHECK(Count(menu, "ImGui::BeginDisabled(!reduced||depthSpatial);") == 1);
        CHECK(Count(menu, "constbooldepthSpatial=depthReadout.valid?depthReadout.active:nr.spatialCompression.value_or_default();") == 1);
        CHECK(Count(menu, "conststd::stringdepthStatus=DepthAwareStatus();") == 1);
        CHECK(Count(menu, "if(!depthStatus.empty())ImGui::TextDisabled(\"%s\",depthStatus.c_str());") == 1);
        // the enlargement mode, the depth-aware checkbox and the downscaler follow the numbers the pass uses: the same function, the keys
        // and the uniform Model resolution as it is on screen, X on the X key and Y on the Y key. The enlargement combo and the checkbox
        // are greyed whenever either scale is above 100% (nothing is enlarged then: a model above 100% is brought back down)
        CHECK(Count(menu, "constfloatuniformShown=pendingScale>=0?pendingScale/100.0f:nr.workingScale.value_or_default();") == 1);
        CHECK(Count(menu, "constAnamorphicLatchrefusal=AnamorphicLatchState();") == 1);
        // one function for the menu and the enlargement key: the keys through the latch, then the governor's run-time cap
        CHECK(Count(menu, "constAxisScalesshownScales=DlssNr::ShownEffectiveScales(uniformShown);") == 1);
        CHECK(Count(menu, "constfloateffectiveScaleX=shownScales.x;") == 1 && Count(menu, "constfloateffectiveScaleY=shownScales.y;") == 1);
        CHECK(Count(menu, "constboolaboveFull=effectiveScaleX>1.0f||effectiveScaleY>1.0f;") == 1);
        CHECK(Count(menu, "if(aboveFull){") == 1);
        // The Enlargement combo is greyed by the one toggle rule the hotkey shares (NrHotkeys.h): the key is a no-op
        // exactly while the combo is greyed, and the menu keeps no grey-out rule of its own.
        CHECK(Count(menu, "constboolreduced=DlssNr::Hotkeys::TransferToggleable(effectiveScaleX,effectiveScaleY);") == 1);
        CHECK(!Has(menu, "EffectiveAxisScale(")); // the menu has no scale rule of its own
        CHECK(Count(menu, "if(AnamorphicLatchHolds(refusal,nr.workingScaleX.value_or_default(),nr.workingScaleY.value_or_default(),nr.workingScale.value_or_default()))ImGui::TextDisabled(\"Themodelrefusedasizethatdiffersperaxis:workingatModelresolutiononbothaxes.\");") == 1);
        // the order: the uniform scale and the latch are read before the scales, the scales before the controls that follow them
        {
            const size_t shown = menu.find("constAxisScalesshownScales=DlssNr::ShownEffectiveScales(");
            const size_t downscaler = menu.find("if(aboveFull){");
            const size_t reducedAt = menu.find("constboolreduced=DlssNr::Hotkeys::TransferToggleable(");
            const size_t checkbox = menu.find("ImGui::BeginDisabled(!reduced||depthSpatial);");
            CHECK(shown != std::string::npos && downscaler != std::string::npos && reducedAt != std::string::npos && checkbox != std::string::npos);
            CHECK(shown < downscaler && downscaler < reducedAt && reducedAt < checkbox);
        }
        // the per-axis sliders: each shows its own key over 0..200, 0 reads "0 (same as Model resolution)" (the format keeps a conversion so a
        // typed value is read back), what is typed is shown as what will be applied, and the value is committed when the handle is let go
        CHECK(Count(menu, "intaxisXPercent=pendingAxisX>=0?pendingAxisX:AxisPercentOfScale(nr.workingScaleX.value_or_default());") == 1);
        CHECK(Count(menu, "intaxisYPercent=pendingAxisY>=0?pendingAxisY:AxisPercentOfScale(nr.workingScaleY.value_or_default());") == 1);
        CHECK(Count(menu, "if(ImGui::SliderInt(\"Horizontalmodelresolution\",&axisXPercent,0,200,axisXPercent==0?\"%d(sameasModelresolution)\":\"%d%%\"))pendingAxisX=AxisPercentApplied(axisXPercent);") == 1);
        CHECK(Count(menu, "if(ImGui::SliderInt(\"Verticalmodelresolution\",&axisYPercent,0,200,axisYPercent==0?\"%d(sameasModelresolution)\":\"%d%%\"))pendingAxisY=AxisPercentApplied(axisYPercent);") == 1);
        CHECK(!Has(menu, "\"SameasModelresolution\""));
        CHECK(Count(menu, "if(ImGui::IsItemDeactivatedAfterEdit()&&pendingAxisX>=0){nr.workingScaleX=AxisScaleFromPercent(pendingAxisX);pendingAxisX=-1;}") == 1);
        CHECK(Count(menu, "if(ImGui::IsItemDeactivatedAfterEdit()&&pendingAxisY>=0){nr.workingScaleY=AxisScaleFromPercent(pendingAxisY);pendingAxisY=-1;}") == 1);
        // the uniform Model resolution slider: 25..200, committed on release, never below 25
        CHECK(Count(menu, "ImGui::SliderInt(\"Modelresolution\",&scalePercent,25,200,\"%d%%\")") == 1);
        CHECK(Count(menu, "if(ImGui::IsItemDeactivatedAfterEdit()&&pendingScale>=0){nr.workingScale=std::clamp(pendingScale,25,200)/100.0f;pendingScale=-1;}") == 1);
    }

    // ---- the per-axis refusal logs, the runtime-switch clear, and the enlargement greying and tooltips -----
    // The create-branch warning names the horizontal share then the vertical one (width then height, like the evaluate log).
    CHECK(Count(pass, "geometry.workScaleX*100.0f,geometry.workScaleY*100.0f,initResult,") == 1);
    // The evaluate log warns for the fallback to the uniform size and errors for the session-ending failure.
    CHECK(Count(pass, "if(anamorphicFallback)LOG_WARN(\"DLSS-NRevaluatereturned") == 1);
    // A runtime switch forgets both the spatial fallback and the refused per-axis size. The ClearHistory body clears the
    // latch member and the spatial flag (a whole-file count would also see the new-layout retry, so this is anchored to the function).
    {
        const size_t clearAt = pass.find("voidClearHistory(){");
        const size_t clearEnd = pass.find("voidDropReuseCaches()", clearAt == std::string::npos ? 0 : clearAt);
        CHECK(clearAt != std::string::npos && clearEnd != std::string::npos && clearAt < clearEnd);
        if (clearAt != std::string::npos && clearEnd != std::string::npos && clearAt < clearEnd)
        {
            const std::string clearBody = pass.substr(clearAt, clearEnd - clearAt);
            CHECK(Has(clearBody, "g_nr.spatialFailed=false;") && Has(clearBody, "g_nr.anamorphic={};"));
        }
    }
    // The Enlargement combo is greyed exactly while the model is not reduced (native or above 100%).
    CHECK(Count(menu, "if(!reduced)ImGui::BeginDisabled();") == 1 && Count(menu, "if(!reduced)ImGui::EndDisabled();") == 1);
    // The tooltips keep their above-100% and refusal clauses.
    CHECK(Count(menu, "Noeffectat100%orabove,norwhileanaxisisabove100%(greyedout).") == 1);
    CHECK(Count(menu, "Doesnothingat100%orabove,norwhileanaxisisabove100%(greyedout),andisoffwhile") == 1);
    CHECK(Count(menu, "Ifthemodelrefusesasizethatdiffersperaxis,thepassworksatModelresolutiononbothaxesuntilascalesettingchanges.") == 1);

    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: foveated and settle wiring (one geometry choice, partial park, per-resource allocation, both shaders branch on the profile, decision inputs, menu placement and readout)");
    return 0;
}
