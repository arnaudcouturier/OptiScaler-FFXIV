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
    CHECK(Has(pass, "constboolimmediate=!consistent||g_nr.spatialFailed||placementChanged||tuningChanged;"));
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
    const size_t workBlock = pass.find("if(DlssNr::Settle::ParksWorkScratch(parkScope)){");
    CHECK(frameBlock != std::string::npos && workBlock != std::string::npos && workBlock < frameBlock);
    if (frameBlock != std::string::npos && workBlock != std::string::npos)
    {
        const std::string work = pass.substr(workBlock, frameBlock - workBlock);
        const std::string frameScratch = pass.substr(frameBlock, 300);
        CHECK(Has(work, "ParkNrResource(g_nr.output);"));
        CHECK(Has(work, "&g_nr.spatialColor,&g_nr.spatialDepth,&g_nr.spatialMotion,&g_nr.spatialProxy,&g_nr.spatialAnswer"));
        CHECK(!Has(work, "g_nr.colorCopy") && !Has(work, "g_nr.hdrCopy") && !Has(work, "g_nr.activeColor"));
        CHECK(Has(frameScratch, "ParkNrResource(g_nr.colorCopy);") && Has(frameScratch, "ParkNrResource(g_nr.hdrCopy);"));
        CHECK(Has(frameScratch, "ParkNrResource(g_nr.activeColor);") && Has(frameScratch, "ParkNrResource(g_nr.outputNative);"));
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

    // The output size the geometry is built for is the colour allocation (`desc`), never the active rectangle (`width`/`height`):
    // the Foveated pixel inputs are output pixels and the pass converts them to the model's frame (pre-SR: the frame is smaller).
    CHECK(Count(pass, "MakeGeometry(requestedSettings,width,height,(unsignedint)desc.Width,(unsignedint)desc.Height,nr.workingScale.value_or_default(),g_nr.spatialFailed);") == 1);
    CHECK(Count(pass, "MakeGeometry(requestedSettings,width,height,(unsignedint)desc.Width,(unsignedint)desc.Height,nr.workingScale.value_or_default(),false);") == 1);
    // and the menu readout reports that same output size, width then height
    CHECK(Count(pass, "requested.workW,requested.workH,(unsignedint)desc.Width,(unsignedint)desc.Height};") == 1);
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

    if (failures != 0)
    {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS: foveated and settle wiring (one geometry choice, partial park, per-resource allocation, both shaders branch on the profile, decision inputs, menu placement and readout)");
    return 0;
}
