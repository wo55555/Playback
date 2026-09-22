#include "OfflineRenderTrace.h"

#include "playback/visuals/FrameCaptureTypes.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace playback::exporting {
namespace {

constexpr size_t MaxEvents  = 1 << 18;
constexpr size_t EventCount = static_cast<size_t>(OfflineRenderTraceEvent::Count);

constexpr std::array EventNames{
    "SessionBegin",
    "SessionEnd",
    "ClockPublished",
    "ClockPermitted",
    "ClockCleared",
    "GraphicsEnter",
    "GraphicsExit",
    "GraphicsSkipped",
    "GameRenderEnter",
    "GameRenderExit",
    "ApiFrameEnter",
    "ApiFrameExit",
    "RenderFrameEnter",
    "RenderFrameExit",
    "SubmitD3D12Enter",
    "SubmitD3D12Exit",
    "SubmitD3D11Enter",
    "SubmitD3D11Exit",
    "PresentEnter",
    "PresentExit",
    "Present1Enter",
    "Present1Exit",
    "CaptureArm",
    "CaptureGate",
    "CaptureSource",
    "CaptureRecorded",
    "CaptureFence",
    "ReadbackReady",
    "CaptureCollected",
    "WarmupComplete",
    "BoundaryFault",
    "UpscalingEnter",
    "UpscalingExit",
    "UpscalingState",
    "HookInventory",
    "RenderState",
    "ClientRenderState",
    "ResourceState",
    "PreviewSource",
    "PreviewCopy",
    "PreviewDisplay",
    "UpscalingConfig",
    "CameraState",
    "CameraLens",
    "CameraSample",
    "SubmissionGate",
    "ExtractFrameEnter",
    "ExtractFrameExit",
    "ExtractedFrame",
    "ExtractedView",
    "GameEndFrameEnter",
    "GameEndFrameExit",
    "ReadbackCopy",
    "CapturePixelLayout",
    "CapturePixels",
    "CapturePixelsInvalid",
    "CpuSubmissionPermit",
    "CaptureLuma",
    "CaptureLumaDistribution",
    "CaptureLumaTile",
    "WorldEnvironment",
    "WorldClock",
    "WeatherRain",
    "WeatherLightning",
    "WeatherSample",
    "ClockSample",
    "GraphicsTimer",
    "GraphicsTimerAbsolute",
    "CameraForward",
    "CameraUp",
    "CameraViewForward",
    "CameraViewUp",
    "DriverTickEnter",
    "DriverTickExit",
    "DriverStage",
    "DriverCollectEnter",
    "DriverCollectExit",
    "DriverAdvanceEnter",
    "DriverAdvanceExit",
    "BoundaryState",
    "WriterSubmitEnter",
    "WriterSubmitExit",
    "WriterQueue",
    "WriterWorkEnter",
    "WriterWorkExit",
    "WriterNormalizeEnter",
    "WriterNormalizeExit",
    "WriterPipeEnter",
    "WriterPipeExit",
    "WriterLaunchEnter",
    "WriterLaunchExit",
    "DriverWait",
    "GraphicsDecision",
    "RendererClock",
    "DiagnosticWindow",
    "ReadbackFenceObserved",
    "ReadbackMapEnter",
    "ReadbackMapExit",
    "ReadbackCpuCopyEnter",
    "ReadbackCpuCopyExit",
    "WriterPipeTiming",
    "WriterPipeChunks",
    "ExtractedViewInputs",
    "RendererClockIdentity",
    "FrameBuilderTiming",
};
static_assert(EventNames.size() == EventCount);

constexpr uint32_t IdleStride = 32;
constexpr uint32_t HotBudget  = 96;

// Retention and opening a hot window are separate decisions.
constexpr bool isSignificant(OfflineRenderTraceEvent event) noexcept {
    switch (event) {
    case OfflineRenderTraceEvent::SessionBegin:
    case OfflineRenderTraceEvent::SessionEnd:
    case OfflineRenderTraceEvent::ClockPublished:
    case OfflineRenderTraceEvent::ClockCleared:
    case OfflineRenderTraceEvent::GraphicsEnter:
    case OfflineRenderTraceEvent::GraphicsExit:
    case OfflineRenderTraceEvent::GameRenderEnter:
    case OfflineRenderTraceEvent::GameRenderExit:
    case OfflineRenderTraceEvent::CaptureArm:
    case OfflineRenderTraceEvent::CaptureSource:
    case OfflineRenderTraceEvent::CaptureRecorded:
    case OfflineRenderTraceEvent::CaptureFence:
    case OfflineRenderTraceEvent::ReadbackReady:
    case OfflineRenderTraceEvent::CaptureCollected:
    case OfflineRenderTraceEvent::WarmupComplete:
    case OfflineRenderTraceEvent::BoundaryFault:
    case OfflineRenderTraceEvent::UpscalingEnter:
    case OfflineRenderTraceEvent::UpscalingExit:
    case OfflineRenderTraceEvent::UpscalingState:
    case OfflineRenderTraceEvent::HookInventory:
    case OfflineRenderTraceEvent::RenderState:
    case OfflineRenderTraceEvent::ClientRenderState:
    case OfflineRenderTraceEvent::ResourceState:
    case OfflineRenderTraceEvent::UpscalingConfig:
    case OfflineRenderTraceEvent::CameraState:
    case OfflineRenderTraceEvent::CameraLens:
    case OfflineRenderTraceEvent::CameraSample:
    case OfflineRenderTraceEvent::SubmissionGate:
    case OfflineRenderTraceEvent::ExtractFrameEnter:
    case OfflineRenderTraceEvent::ExtractFrameExit:
    case OfflineRenderTraceEvent::ExtractedFrame:
    case OfflineRenderTraceEvent::ExtractedView:
    case OfflineRenderTraceEvent::GameEndFrameEnter:
    case OfflineRenderTraceEvent::GameEndFrameExit:
    case OfflineRenderTraceEvent::ReadbackCopy:
    case OfflineRenderTraceEvent::CapturePixelLayout:
    case OfflineRenderTraceEvent::CapturePixels:
    case OfflineRenderTraceEvent::CapturePixelsInvalid:
    case OfflineRenderTraceEvent::CpuSubmissionPermit:
    case OfflineRenderTraceEvent::CaptureLuma:
    case OfflineRenderTraceEvent::CaptureLumaDistribution:
    case OfflineRenderTraceEvent::CaptureLumaTile:
    case OfflineRenderTraceEvent::WorldEnvironment:
    case OfflineRenderTraceEvent::WorldClock:
    case OfflineRenderTraceEvent::WeatherRain:
    case OfflineRenderTraceEvent::WeatherLightning:
    case OfflineRenderTraceEvent::WeatherSample:
    case OfflineRenderTraceEvent::ClockSample:
    case OfflineRenderTraceEvent::GraphicsTimer:
    case OfflineRenderTraceEvent::GraphicsTimerAbsolute:
    case OfflineRenderTraceEvent::CameraForward:
    case OfflineRenderTraceEvent::CameraUp:
    case OfflineRenderTraceEvent::CameraViewForward:
    case OfflineRenderTraceEvent::CameraViewUp:
    case OfflineRenderTraceEvent::DriverTickEnter:
    case OfflineRenderTraceEvent::DriverTickExit:
    case OfflineRenderTraceEvent::DriverStage:
    case OfflineRenderTraceEvent::DriverCollectEnter:
    case OfflineRenderTraceEvent::DriverCollectExit:
    case OfflineRenderTraceEvent::DriverAdvanceEnter:
    case OfflineRenderTraceEvent::DriverAdvanceExit:
    case OfflineRenderTraceEvent::BoundaryState:
    case OfflineRenderTraceEvent::WriterSubmitEnter:
    case OfflineRenderTraceEvent::WriterSubmitExit:
    case OfflineRenderTraceEvent::WriterQueue:
    case OfflineRenderTraceEvent::WriterWorkEnter:
    case OfflineRenderTraceEvent::WriterWorkExit:
    case OfflineRenderTraceEvent::WriterNormalizeEnter:
    case OfflineRenderTraceEvent::WriterNormalizeExit:
    case OfflineRenderTraceEvent::WriterPipeEnter:
    case OfflineRenderTraceEvent::WriterPipeExit:
    case OfflineRenderTraceEvent::WriterLaunchEnter:
    case OfflineRenderTraceEvent::WriterLaunchExit:
    case OfflineRenderTraceEvent::DriverWait:
    case OfflineRenderTraceEvent::GraphicsDecision:
    case OfflineRenderTraceEvent::RendererClock:
    case OfflineRenderTraceEvent::DiagnosticWindow:
    case OfflineRenderTraceEvent::ReadbackFenceObserved:
    case OfflineRenderTraceEvent::ReadbackMapEnter:
    case OfflineRenderTraceEvent::ReadbackMapExit:
    case OfflineRenderTraceEvent::ReadbackCpuCopyEnter:
    case OfflineRenderTraceEvent::ReadbackCpuCopyExit:
    case OfflineRenderTraceEvent::WriterPipeTiming:
    case OfflineRenderTraceEvent::WriterPipeChunks:
    case OfflineRenderTraceEvent::ExtractedViewInputs:
    case OfflineRenderTraceEvent::RendererClockIdentity:
    case OfflineRenderTraceEvent::FrameBuilderTiming:
        return true;
    default:
        return false;
    }
}

struct TraceEvent {
    uint64_t                sequence{};
    uint64_t                us{};
    uint64_t                thread{};
    OfflineRenderTraceEvent event{};
    uint64_t                parent{};
    uint64_t                scopedToken{};
    uint64_t                scopedFrame{};
    uint64_t                renderSerial{};
    uint64_t                observedToken{};
    uint64_t                observedFrame{};
    uintptr_t               object{};
    uintptr_t               related{};
    uint64_t                a{};
    uint64_t                b{};
    uint64_t                c{};
    uint64_t                d{};
};

struct ThreadContext {
    uint64_t                               epoch{};
    uint64_t                               parent{};
    uint64_t                               token{};
    uint64_t                               frame{};
    uint64_t                               renderSerial{};
    std::array<std::array<uint64_t, 7>, 3> lastObservation{};
    std::array<bool, 3>                    hasObservation{};
};

struct TraceState {
    std::mutex                            controlMutex;
    std::mutex                            recordMutex;
    std::atomic<uint64_t>                 activeEpoch{};
    uint64_t                              lastEpoch{};
    std::FILE*                            file{};
    std::vector<TraceEvent>               events;
    size_t                                stored{};
    uint64_t                              dropped{};
    std::array<uint64_t, EventCount>      totals{};
    std::array<uint64_t, EventCount>      idleSeen{};
    std::chrono::steady_clock::time_point started{};
    uint64_t                              observedToken{};
    uint64_t                              observedFrame{};
    uint32_t                              hotBudget{};
    uint64_t                              thinned{};
};

TraceState                 gTrace;
thread_local ThreadContext gThread;

bool validEvent(OfflineRenderTraceEvent event) noexcept { return static_cast<size_t>(event) < EventCount; }

void synchronizeThread(uint64_t epoch) noexcept {
    if (gThread.epoch != epoch) {
        gThread       = {};
        gThread.epoch = epoch;
    }
}

enum class TraceStorage { Sample, Keep, Suppress };

// The caller holds recordMutex and has checked the captured epoch.
uint64_t appendEvent(
    OfflineRenderTraceEvent event,
    void const*             object,
    void const*             related,
    uint64_t                a,
    uint64_t                b,
    uint64_t                c,
    uint64_t                d,
    TraceStorage            storage = TraceStorage::Sample
) noexcept {
    auto const index = static_cast<size_t>(event);
    ++gTrace.totals[index];
    bool verboseSampled = false;
    if (storage == TraceStorage::Sample) {
        size_t slot = 3;
        if (event == OfflineRenderTraceEvent::DriverWait) slot = 0;
        if (event == OfflineRenderTraceEvent::GraphicsDecision) slot = 1;
        if (event == OfflineRenderTraceEvent::RenderState && (a == 14 || a == 15)) slot = 2;
        if (slot < 3) {
            std::array<uint64_t, 7> const value{
                reinterpret_cast<uintptr_t>(object),
                reinterpret_cast<uintptr_t>(related),
                slot == 2 ? 0 : a,
                b,
                c,
                d,
                gThread.token
            };
            if (gThread.hasObservation[slot] && gThread.lastObservation[slot] == value) {
                ++gTrace.thinned;
                return 0;
            }
            gThread.lastObservation[slot] = value;
            gThread.hasObservation[slot]  = true;
        }
        bool const verbose =
            event == OfflineRenderTraceEvent::PreviewSource || event == OfflineRenderTraceEvent::PreviewCopy
            || event == OfflineRenderTraceEvent::PreviewDisplay || event == OfflineRenderTraceEvent::CaptureGate
            || event == OfflineRenderTraceEvent::ClockPermitted;
        if (verbose && gThread.token == 0) {
            verboseSampled = true;
            if (++gTrace.idleSeen[index] % IdleStride != 0) {
                ++gTrace.thinned;
                return 0;
            }
        }
    }
    if (storage == TraceStorage::Suppress) {
        if (gTrace.stored == MaxEvents) {
            ++gTrace.dropped;
        } else {
            ++gTrace.thinned;
        }
        return 0;
    }
    if (event == OfflineRenderTraceEvent::ClockPublished || event == OfflineRenderTraceEvent::CaptureArm
        || event == OfflineRenderTraceEvent::CpuSubmissionPermit || event == OfflineRenderTraceEvent::SessionBegin
        || event == OfflineRenderTraceEvent::BoundaryFault || event == OfflineRenderTraceEvent::DiagnosticWindow) {
        gTrace.hotBudget = HotBudget;
    } else if (!isSignificant(event) && storage == TraceStorage::Sample && !verboseSampled) {
        if (gTrace.hotBudget != 0) {
            --gTrace.hotBudget;
        } else if (++gTrace.idleSeen[index] % IdleStride != 0) {
            ++gTrace.thinned;
            return 0;
        }
    }
    if (gTrace.stored == MaxEvents) {
        ++gTrace.dropped;
        return 0;
    }

    auto const sequence = static_cast<uint64_t>(gTrace.stored) + 1;
    auto const us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - gTrace.started)
            .count();
    gTrace.events[gTrace.stored++] = {
        sequence,
        static_cast<uint64_t>(us),
        static_cast<uint64_t>(GetCurrentThreadId()),
        event,
        gThread.parent,
        gThread.token,
        gThread.frame,
        gThread.renderSerial,
        gTrace.observedToken,
        gTrace.observedFrame,
        reinterpret_cast<uintptr_t>(object),
        reinterpret_cast<uintptr_t>(related),
        a,
        b,
        c,
        d,
    };
    return sequence;
}

bool writeCsv(
    std::FILE*                              file,
    uint64_t                                session,
    std::vector<TraceEvent> const&          events,
    size_t                                  stored,
    uint64_t                                dropped,
    uint64_t                                thinned,
    std::array<uint64_t, EventCount> const& totals
) noexcept {
    bool okay =
        std::fputs(
            "# object/related are opaque pointer values, never dereferenced; not proof of identity or lifetime.\n"
            "# observed_token/observed_frame are global observations, not proof of source or causality.\n"
            "# scoped_token/scoped_frame/render_serial are explicit Graphics-thread TLS, never propagated.\n"
            "# parent is the nearest stored enclosing enter; an exit references its own enter; zero means none.\n"
            "# us is steady-clock microseconds since begin; session is a process-local epoch.\n"
            "# count totals are exact; repeated decisions/states and verbose preview/polls are thinned.\n"
            "# DriverWait/GraphicsDecision keep changes per thread; graphics RenderState keeps changed payloads.\n"
            "# A retained wait is the last observed branch, not proof it remains blocked between driver calls.\n"
            "# Scope exits inherit entry sampling; buffer exhaustion or session end can still truncate scopes.\n"
            "# Events remain buffered until finish; no crash safety or durable-storage guarantee.\n"
            "# RenderState: a=stage,b=flags,c=width<<32|height,d=upscale factor float bits; object=FrameBuilder.\n"
            "# RenderState flags: 1=present,2=initialized,4=enabled,8=deferred capable,16=deferred enabled,"
            "32=raytracing capable,64=raytracing enabled,128=upscaling available,256=upscaling enabled.\n"
            "# RenderState flags are renderer queries, not proof that a particular GPU pass ran.\n"
            "# Stages: 1=ReplayStart,2=ReplayPacksPrepared,3=ReplayWorldJoined,4=ReplayWorldReady,5=EditorShown,"
            "6=EditorHidden,7=ClientPulse,8=ExportBeforeOpen,9=ExportAfterOpen,10=ResizeBefore,11=ResizeAfter,"
            "12=RestoreBefore,13=RestoreAfter,14=GraphicsBefore,15=GraphicsAfter,16=ExportClosed.\n"
            "# ClientRenderState: a=stage,b=viewport width float bits,c=height float bits,d=replay tick.\n"
            "# ResourceState: a=stage,b=capability flags (1=VV supported,2=pbr,4=raytraced),c=resources loaded,d=pack "
            "count.\n"
            "# Resource capabilities do not imply an enabled rendering mode.\n"
            "# PreviewSource: object=backbuffer,related=swapchain,a=source serial,b=width,c=height,d=format.\n"
            "# PreviewCopy: object=source,related=copy,a=source serial,b=copy slot,c=backend 11/12,d=capture "
            "requested.\n"
            "# PreviewDisplay: object=copy,related=source,a=content serial,b=selected slot,c=backend,d=export "
            "overlay.\n"
            "# CaptureSource: object=source,related=queue/context,a=source serial,b=source slot,c=backend,d=format.\n"
            "# Preview serials describe CPU-recorded work, not GPU completion or replay sample identity.\n"
            "# UpscalingConfig: a=mode,b=dynamic enabled,c=dynamic scale float bits,d=upscale factor float bits.\n"
            "# CameraState: object=camera,related=level renderer,a/b/c=position float bits,d=context frameIndex or "
            "zero.\n"
            "# CameraLens: a=fov float bits,b=aspect float bits,c=partialTick float bits,d=0 native/1 preview/2 "
            "export/3 parked.\n"
            "# CameraSample: a=sample tick double bits,b=context frameIndex,c=source marker,d=0; only with valid "
            "sample.\n"
            "# WarmupComplete: a=completed warmup renders,b=configured warmup frames.\n"
            "# CaptureArm: a=frame index,b=armed,c=observed sample generation; arm retries do not change it.\n"
            "# CpuSubmissionPermit: a=sample generation,b=clock token,c=render serial,d=CAS succeeded. "
            "Recorded after authorization, not the exact CAS timestamp or GPU completion.\n"
            "# SubmissionGate: object=BGFX frame,related=renderer,a=entry generation,b=observed return generation,"
            "c=scene bit1|CPU-entry-eligible bit2,d=accepted; this is a gate observation, not native frame identity.\n"
            "# HookInventory: a=renderFrame hook,b=upscaling hook,c=warmup frames,d=CPU diagnostic hook mask "
            "(1=extractFrame,2=endFrame).\n"
            "# ExtractFrameEnter/Exit: object=GameRenderer,related=ScreenContext,b=extraction serial,c=play screen; "
            "exit a=returned non-null. No Graphics TLS means sample identity is unknown.\n"
            "# ExtractedFrame: object=FrameRenderObject,related=GameRenderer,a=extraction serial,b=view count,"
            "c=play screen,d=returned non-null. CPU extraction is not native GPU frame identity.\n"
            "# ExtractedView: object=view,related=FrameRenderObject,a=index<<1|shadow pass,b/c/d=camera xyz float "
            "bits; at most first 4 views, parent=extract span.\n"
            "# GameEndFrameEnter/Exit: object=GameRenderer,related=RenderContext; CPU boundary, not GPU completion.\n"
            "# ReadbackCopy: object=source,related=readback,a=frame index,b=capture ID,c=command list address,"
            "d=queue address; records CopyTextureRegion, not execution or completion.\n"
            "# CapturePixelLayout: object=source,related=completion fence,a=frame index,b=width<<32|height,"
            "c=row pitch,d=pixel format (0=RGBA8,1=BGRA8). Taken after collection, before normalization/encoding.\n"
            "# CapturePixels: same objects,a=frame index,b=RGB minima packed R|G<<8|B<<16,c=RGB maxima,d=FNV1a64 "
            "of canonical RGB sampled on min(width,32)*min(height,18) endpoint-inclusive grid. "
            "Excludes alpha/padding; sampled uniformity is not full-image uniformity or scene readiness.\n"
            "# CapturePixelsInvalid: a=frame index,b=width<<32|height,c=row pitch,d=available bytes; probe only.\n"
            "# CaptureLuma: same source/fence and grid as CapturePixels; a=frame index,b=sample count,c=sumY,d=sumY2. "
            "Y=(54R+183G+19B+128)>>8 is an 8-bit display-code brightness proxy, not linear luminance or exposure.\n"
            "# CaptureLumaDistribution: a=frame index,b=min|p10<<8|p50<<16|p90<<24|max<<32,"
            "c=count(Y<=8),d=count(Y>=247); percentiles use nearest rank ceil(p*count/100).\n"
            "# CaptureLumaTile: a=frame index,b=tile index [0,8] row-major,c=sample count,d=sumY; "
            "tile=3*floor(3*gridRow/rows)+floor(3*gridColumn/columns); empty tiles have count=sum=0. "
            "Screen-space samples are not motion compensated; composition changes can change brightness.\n"
            "# World stages: 1=before replay tick,2=before native subTick,3=after native subTick,"
            "4=before graphics origin,5=after graphics origin,6=after native camera setup.\n"
            "# WorldEnvironment: object=level,related=dimension,a=world stage,b=available flags "
            "(1=level,2=dimension,4=weather),c=partialTick float bits (0 placeholder at tick stages),"
            "d=offline tick token (0 outside tick). "
            "Tick/graphics stages query only level; dimension/weather are sampled at camera setup.\n"
            "# WorldClock: same objects,a=world stage,b=Level.getTime(),c=applied replay tick,d=offline tick token. "
            "Absent when level unavailable; signed integers are converted through int64 to uint64. "
            "Offline tick tokens are not Graphics clock tokens or frame identity.\n"
            "# WeatherRain/WeatherLightning: object=weather,related=dimension,a=world stage,"
            "b/c/d=old/current/target float bits, not an exposure reading.\n"
            "# WeatherSample: object=weather,related=dimension,a=world stage,b=rain getter float bits,"
            "c=lightning getter float bits,d=skyFlashTime signed integer; getters use WorldEnvironment.partialTick. "
            "Unavailable weather emits no weather events; getters do not prove final GPU constants.\n"
            "# ClockSample: a=frame index,b=planned replay tick double bits,c=planned deltaTicks float bits,"
            "d=planned wholeTicks; TLS token identifies the invocation, formal samples correlate with "
            "CpuSubmissionPermit; warmup can reuse frame index 0.\n"
            "# GraphicsTimer: object=Timer,related=client,a=phase (1=before override,2=before origin,"
            "3=after origin,4=after restoration),b=alpha float bits,c=lastTimestep seconds float bits,"
            "d=passedTime ticks float bits; actual CPU Timer fields, not asynchronous renderer clocks.\n"
            "# GraphicsTimerAbsolute: same objects/phase,b=ticks signed integer,c=lastTimeSeconds float bits,"
            "d=timeScale float bits.\n"
            "# CameraForward/CameraUp/CameraViewForward/CameraViewUp: object=camera,related=level renderer,"
            "a=phase (0=native setup before override,1=effective native,2=effective preview,3=effective export,"
            "4=effective parked),b/c/d=xyz float bits. Raw public forward/up and inverse-view -column2/column1; "
            "not normalized, not planned angles or final GPU view identity.\n"
            "# DriverTickEnter/Exit: object=driver,b=entry next frame,c=entry ready queue size,d=entry phase "
            "(0=idle,1=rendering,2=draining,3=finalizing,4=cancelling,5=faulted,6=completed,7=cancelled); "
            "exit a=reason (0=other/terminal/unwinding,1=writer backpressure,2=boundary waiting,3=capture backpressure,"
            "4=submission/boundary failure,5=draining,6=diagnostic gap). Gaps between spans are outside driver.tick.\n"
            "# DriverStage: object=driver,a=stage,b=next frame; stage1 after coordinator.status: "
            "c=submitted,d=written; stage2 after boundary.status: c=boundary state,d=ready queue size.\n"
            "# DriverCollectEnter/Exit: object=driver,b=entry next frame,c=entry ready size; "
            "exit a=submission result (0=ready,1=backpressured,2=failed).\n"
            "# DriverAdvanceEnter/Exit: object=boundary,b=requested frame; "
            "exit a=step result (0=waiting,1=backpressured,2=submitted,3=failed).\n"
            "# BoundaryState: object=boundary,a=requested frame,b=entry state "
            "(0=closed,1=ready,2=initializing,3=preparing,4=warming,5=awaiting "
            "download,6=draining,7=cancelled,8=faulted),"
            "c=flags (1=tick pending,2=clock present,4=capture armed,8=completed ticket pending),d=warmup remaining.\n"
            "# WriterSubmitEnter/Exit: object=driver,related=coordinator,b=frame,c=entry ready queue size; "
            "exit a=FrameWriterSubmitResult (0=accepted,1=backpressured,2=closed,3=failed); includes coordinator "
            "locks.\n"
            "# DriverCollect/DriverAdvance/WriterSubmit exit a=UINT64_MAX means no normal result (unwinding).\n"
            "# WriterQueue: object=FFmpeg impl,a=frame,b=queue size,c=capacity,d=action "
            "(1=accepted after enqueue,2=backpressured,3=dequeued); size excludes the worker's current item.\n"
            "# WriterWorkEnter/Exit: object=FFmpeg impl,b=frame,c=queue size at dequeue,d=capacity; "
            "exit a=1 only after successful pipe write and written-count increment, otherwise 0.\n"
            "# WriterNormalizeEnter/Exit,WriterPipeEnter/Exit,WriterLaunchEnter/Exit: object=FFmpeg impl,b=frame; "
            "exit a=success; pipe c=byte count. Pipe span covers the entire frame, not just one WriteFile.\n"
            "# Writer spans are CPU observations, not encoder/GPU completion; worker TLS is not inherited. "
            "Entry/exit us include scheduling and instrumentation; session close may truncate worker spans.\n"
            "# DriverWait: object=driver,a=reason,b=next frame,c=ready head frame or UINT64_MAX,d=driver phase.\n"
            "# Wait reasons: 0=None,1=WriterBackpressure,2=CaptureCapacity,3=ReplayPreparation,"
            "4=DimensionTransition,5=UiStable,6=NativeTick,7=WarmupCpu,8=WarmupBudget,9=WarmupUi,"
            "10=CaptureArm,11=CpuSample,12=CapturePending,13=CollectPending,14=Draining,"
            "15=DiagnosticGap,16=Failed,17=Unknown; one branch per observation, not exclusive root causes.\n"
            "# GraphicsDecision: a=1 SampleExecuted/2 ExportSkipped/3 NativeExecuted,b=explicit sample token,"
            "c=explicit sample frame,d=render serial; zero outside sample. Exceptions have no success decision.\n"
            "# RendererClock: object=GameRenderer,a=1 RenderBefore/2 RenderAfter/3 ExtractBefore/4 ExtractAfter/"
            "5 EndBefore/6 EndAfter,b=mLastClockTime float bits,c=mLastFrameTime nanoseconds signed bits,"
            "d=_tick signed bits; raw CPU fields, not verified shader time or GPU history counters.\n"
            "# ExtractedViewInputs: object=ViewRenderObject,related=FrameRenderObject,a=viewIndex<<8|kind,"
            "kind0=b=FakeHDR,c=SkyBrightnessScalar,d=flags; kind1/2=b/c/d=RGB float bits for fog/sky colors.\n"
            "# RendererClockIdentity: object=GameRenderer mClock,related=ScreenContext clock,a=extract stage,"
            "b=address equality; pointer identity is an observation, not proof of clock consumers.\n"
            "# FrameBuilderTiming: object=FrameBuilder,related=GameRenderer,a=stage,b/c/d=double bit patterns for "
            "render-thread duration,wait-until-completed duration,flip timestamp; units follow FrameBuilder "
            "declaration.\n"
            "# DiagnosticWindow: a=0 planned/1 begin/2 released/3 aborted/4 not reached/5 ineligible,"
            "b=next frame,c=target gap us,d=actual elapsed us on release only; synthetic gap, not writer "
            "backpressure.\n"
            "# ReadbackFenceObserved: object=readback,related=fence,a=frame,b=capture ID,c=fence value,"
            "d=wait API used; CPU observation after existing wait, not a GPU timestamp.\n"
            "# ReadbackMapEnter/Exit,ReadbackCpuCopyEnter/Exit: object=readback,related=fence,b=frame,"
            "c=capture ID,d=fence value; Map exit a=HRESULT uint32 bits or UINT64_MAX unwinding;"
            "CpuCopy exit a=1 copied/0 incomplete, excluding frameTap.complete.\n"
            "# WriterPipeTiming: a=frame,b=mutex-section total ns,c=WriteFile total ns,d=max chunk WriteFile ns.\n"
            "# WriterPipeChunks: a=frame,b=WriteFile calls,c=successful bytes,d=success; timings include OS "
            "scheduling.\n"
            "session,sequence,us,thread,event,parent,scoped_token,scoped_frame,render_serial,"
            "observed_token,observed_frame,object,related,a,b,c,d\n",
            file
        )
        >= 0;

    for (size_t i = 0; i < stored && okay; ++i) {
        auto const& event = events[i];
        okay              = std::fprintf(
                                file,
                                "%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                                ",%" PRIu64 ",%" PRIu64 ",0x%" PRIxPTR ",0x%" PRIxPTR ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                                "\n",
                                session,
                                event.sequence,
                                event.us,
                                event.thread,
                                EventNames[static_cast<size_t>(event.event)],
                                event.parent,
                                event.scopedToken,
                                event.scopedFrame,
                                event.renderSerial,
                                event.observedToken,
                                event.observedFrame,
                                event.object,
                                event.related,
                                event.a,
                                event.b,
                                event.c,
                                event.d
                            )
                         >= 0;
    }
    if (std::fprintf(file, "# dropped,%" PRIu64 "\n# thinned,%" PRIu64 "\n", dropped, thinned) < 0) {
        okay = false;
    }
    for (size_t i = 0; i < EventCount; ++i) {
        if (std::fprintf(file, "# count,%s,%" PRIu64 "\n", EventNames[i], totals[i]) < 0) {
            okay = false;
        }
    }
    if (std::fflush(file) != 0) {
        okay = false;
    }
    if (std::ferror(file) != 0) {
        okay = false;
    }
    if (std::fclose(file) != 0) {
        okay = false;
    }
    return okay;
}

} // namespace

bool beginOfflineRenderTrace(std::filesystem::path const& path) noexcept {
    try {
        std::lock_guard controlLock(gTrace.controlMutex);
        if (gTrace.activeEpoch.load(std::memory_order_acquire) != 0
            || gTrace.lastEpoch == (std::numeric_limits<uint64_t>::max)()) {
            return false;
        }

        auto const filename = path.string();
        if (filename.empty() || filename.find('\0') != std::string::npos || std::filesystem::path(filename) != path) {
            return false;
        }
        std::vector<TraceEvent> events;
        events.reserve(MaxEvents);
        events.resize(MaxEvents);
        std::FILE* rawFile{};
        if (fopen_s(&rawFile, filename.c_str(), "wbx") != 0 || !rawFile) {
            return false;
        }
        std::unique_ptr<std::FILE, decltype(&std::fclose)> file(rawFile, &std::fclose);

        std::lock_guard recordLock(gTrace.recordMutex);
        gTrace.events.swap(events);
        gTrace.stored        = 0;
        gTrace.dropped       = 0;
        gTrace.thinned       = 0;
        gTrace.totals        = {};
        gTrace.idleSeen      = {};
        gTrace.hotBudget     = 0;
        gTrace.observedToken = 0;
        gTrace.observedFrame = 0;
        gTrace.started       = std::chrono::steady_clock::now();
        gTrace.file          = file.release();
        gTrace.activeEpoch.store(++gTrace.lastEpoch, std::memory_order_release);
        return true;
    } catch (...) {
        return false;
    }
}

bool finishOfflineRenderTrace() noexcept {
    try {
        std::lock_guard                  controlLock(gTrace.controlMutex);
        std::vector<TraceEvent>          events;
        std::array<uint64_t, EventCount> totals{};
        std::FILE*                       file{};
        uint64_t                         session{};
        size_t                           stored{};
        uint64_t                         dropped{};
        uint64_t                         thinned{};
        {
            std::lock_guard recordLock(gTrace.recordMutex);
            session = gTrace.activeEpoch.load(std::memory_order_acquire);
            if (session == 0) {
                return true;
            }
            gTrace.activeEpoch.store(0, std::memory_order_release);
            events.swap(gTrace.events);
            totals  = gTrace.totals;
            stored  = gTrace.stored;
            dropped = gTrace.dropped;
            thinned = gTrace.thinned;
            file    = std::exchange(gTrace.file, nullptr);
        }
        return writeCsv(file, session, events, stored, dropped, thinned, totals);
    } catch (...) {
        return false;
    }
}

bool isOfflineRenderTraceActive() noexcept { return gTrace.activeEpoch.load(std::memory_order_acquire) != 0; }

uint64_t offlineRenderTraceEpoch() noexcept { return gTrace.activeEpoch.load(std::memory_order_acquire); }

void recordOfflineRenderPixels(visuals::CapturedFrame const& frame) noexcept {
    auto const epoch = gTrace.activeEpoch.load(std::memory_order_acquire);
    if (epoch == 0) return;

    auto const                dimensions = (static_cast<uint64_t>(frame.width) << 32) | frame.height;
    bool const                valid      = frame.width != 0 && frame.height != 0
                                        && static_cast<uint64_t>(frame.rowPitch) >= static_cast<uint64_t>(frame.width) * 4
                                        && static_cast<uint64_t>(frame.rowPitch) * frame.height <= frame.pixels.size()
                                        && (frame.pixelFormat == visuals::FramePixelFormat::Rgba8
                                            || frame.pixelFormat == visuals::FramePixelFormat::Bgra8);
    uint32_t                  minima     = 0;
    uint32_t                  maxima     = 0;
    uint64_t                  hash       = 14695981039346656037ull;
    uint64_t                  sampleCount{};
    uint64_t                  lumaSum{};
    uint64_t                  lumaSquaredSum{};
    uint64_t                  lumaDistribution{};
    uint64_t                  darkCount{};
    uint64_t                  brightCount{};
    std::array<uint32_t, 256> lumaHistogram{};
    std::array<uint32_t, 9>   tileCounts{};
    std::array<uint64_t, 9>   tileSums{};
    if (valid) {
        std::array<uint8_t, 3> low{255, 255, 255};
        std::array<uint8_t, 3> high{};
        auto const             columns = (std::min)(frame.width, 32u);
        auto const             rows    = (std::min)(frame.height, 18u);
        for (uint32_t row = 0; row < rows; ++row) {
            auto const y = rows == 1 ? 0 : static_cast<uint64_t>(row) * (frame.height - 1) / (rows - 1);
            for (uint32_t column = 0; column < columns; ++column) {
                auto const  x = columns == 1 ? 0 : static_cast<uint64_t>(column) * (frame.width - 1) / (columns - 1);
                auto const* pixel = frame.pixels.data() + static_cast<size_t>(y * frame.rowPitch + x * 4);
                std::array<uint8_t, 3> rgb{};
                for (uint32_t channel = 0; channel < 3; ++channel) {
                    auto const sourceChannel =
                        frame.pixelFormat == visuals::FramePixelFormat::Bgra8 ? 2 - channel : channel;
                    auto const value = std::to_integer<uint8_t>(pixel[sourceChannel]);
                    rgb[channel]     = value;
                    low[channel]     = (std::min)(low[channel], value);
                    high[channel]    = (std::max)(high[channel], value);
                    hash             = (hash ^ value) * 1099511628211ull;
                }
                auto const luma = (54u * rgb[0] + 183u * rgb[1] + 19u * rgb[2] + 128u) >> 8;
                ++sampleCount;
                lumaSum        += luma;
                lumaSquaredSum += static_cast<uint64_t>(luma) * luma;
                ++lumaHistogram[luma];
                darkCount       += luma <= 8;
                brightCount     += luma >= 247;
                auto const tile  = (row * 3 / rows) * 3 + column * 3 / columns;
                ++tileCounts[tile];
                tileSums[tile] += luma;
            }
        }
        for (uint32_t channel = 0; channel < 3; ++channel) {
            minima |= static_cast<uint32_t>(low[channel]) << (channel * 8);
            maxima |= static_cast<uint32_t>(high[channel]) << (channel * 8);
        }
        auto quantile = [&](uint64_t rank) {
            uint64_t count{};
            for (uint32_t value = 0; value < lumaHistogram.size(); ++value) {
                count += lumaHistogram[value];
                if (count >= rank) return static_cast<uint64_t>(value);
            }
            return uint64_t{255};
        };
        lumaDistribution = quantile(1) | (quantile((sampleCount * 10 + 99) / 100) << 8)
                         | (quantile((sampleCount * 50 + 99) / 100) << 16)
                         | (quantile((sampleCount * 90 + 99) / 100) << 24) | (quantile(sampleCount) << 32);
    }

    try {
        std::lock_guard recordLock(gTrace.recordMutex);
        if (gTrace.activeEpoch.load(std::memory_order_acquire) != epoch) return;
        synchronizeThread(epoch);
        auto const* source = frame.submission.exportResource;
        auto const* fence  = frame.submission.completionFence;
        if (!valid) {
            appendEvent(
                OfflineRenderTraceEvent::CapturePixelsInvalid,
                source,
                fence,
                frame.ticket.frameIndex,
                dimensions,
                frame.rowPitch,
                frame.pixels.size()
            );
            return;
        }
        appendEvent(
            OfflineRenderTraceEvent::CapturePixelLayout,
            source,
            fence,
            frame.ticket.frameIndex,
            dimensions,
            frame.rowPitch,
            static_cast<uint64_t>(frame.pixelFormat)
        );
        appendEvent(
            OfflineRenderTraceEvent::CapturePixels,
            source,
            fence,
            frame.ticket.frameIndex,
            minima,
            maxima,
            hash
        );
        appendEvent(
            OfflineRenderTraceEvent::CaptureLuma,
            source,
            fence,
            frame.ticket.frameIndex,
            sampleCount,
            lumaSum,
            lumaSquaredSum
        );
        appendEvent(
            OfflineRenderTraceEvent::CaptureLumaDistribution,
            source,
            fence,
            frame.ticket.frameIndex,
            lumaDistribution,
            darkCount,
            brightCount
        );
        for (size_t tile = 0; tile < tileCounts.size(); ++tile) {
            appendEvent(
                OfflineRenderTraceEvent::CaptureLumaTile,
                source,
                fence,
                frame.ticket.frameIndex,
                tile,
                tileCounts[tile],
                tileSums[tile]
            );
        }
    } catch (...) {}
}

void setOfflineRenderTraceSample(uint64_t token, uint64_t frameIndex) noexcept {
    auto const epoch = gTrace.activeEpoch.load(std::memory_order_acquire);
    if (epoch == 0) {
        return;
    }
    try {
        std::lock_guard recordLock(gTrace.recordMutex);
        if (gTrace.activeEpoch.load(std::memory_order_acquire) != epoch) {
            return;
        }
        gTrace.observedToken = token;
        gTrace.observedFrame = frameIndex;
    } catch (...) {}
}

void recordOfflineRenderTrace(
    OfflineRenderTraceEvent event,
    void const*             object,
    void const*             related,
    uint64_t                a,
    uint64_t                b,
    uint64_t                c,
    uint64_t                d
) noexcept {
    recordOfflineRenderTraceForEpoch(offlineRenderTraceEpoch(), event, object, related, a, b, c, d);
}

void recordOfflineRenderTraceForEpoch(
    uint64_t                epoch,
    OfflineRenderTraceEvent event,
    void const*             object,
    void const*             related,
    uint64_t                a,
    uint64_t                b,
    uint64_t                c,
    uint64_t                d
) noexcept {
    if (epoch == 0 || !validEvent(event)) {
        return;
    }
    try {
        std::lock_guard recordLock(gTrace.recordMutex);
        if (gTrace.activeEpoch.load(std::memory_order_acquire) != epoch) {
            return;
        }
        synchronizeThread(epoch);
        appendEvent(event, object, related, a, b, c, d);
    } catch (...) {}
}

ScopedOfflineRenderTraceSample::ScopedOfflineRenderTraceSample(
    uint64_t token,
    uint64_t frameIndex,
    uint64_t renderSerial
) noexcept {
    auto const epoch = gTrace.activeEpoch.load(std::memory_order_acquire);
    if (epoch == 0) {
        return;
    }
    synchronizeThread(epoch);
    mEpoch                = epoch;
    mPreviousToken        = gThread.token;
    mPreviousFrame        = gThread.frame;
    mPreviousRenderSerial = gThread.renderSerial;
    gThread.token         = token;
    gThread.frame         = frameIndex;
    gThread.renderSerial  = renderSerial;
}

ScopedOfflineRenderTraceSample::~ScopedOfflineRenderTraceSample() noexcept {
    if (mEpoch != 0 && gThread.epoch == mEpoch && gTrace.activeEpoch.load(std::memory_order_acquire) == mEpoch) {
        gThread.token        = mPreviousToken;
        gThread.frame        = mPreviousFrame;
        gThread.renderSerial = mPreviousRenderSerial;
    }
}

OfflineRenderTraceScope::OfflineRenderTraceScope(
    OfflineRenderTraceEvent enter,
    OfflineRenderTraceEvent exit,
    void const*             object,
    void const*             related,
    uint64_t                a,
    uint64_t                b,
    uint64_t                c,
    uint64_t                d,
    uint64_t                expectedEpoch
) noexcept {
    auto const epoch = gTrace.activeEpoch.load(std::memory_order_acquire);
    if (epoch == 0 || (expectedEpoch != UINT64_MAX && expectedEpoch != epoch) || !validEvent(enter)
        || !validEvent(exit)) {
        return;
    }
    try {
        std::lock_guard recordLock(gTrace.recordMutex);
        if (gTrace.activeEpoch.load(std::memory_order_acquire) != epoch) {
            return;
        }
        synchronizeThread(epoch);
        mEpoch              = epoch;
        mPreviousParent     = gThread.parent;
        mObject             = object;
        mRelated            = related;
        mB                  = b;
        mC                  = c;
        mD                  = d;
        mExit               = exit;
        auto const sequence = appendEvent(enter, object, related, a, b, c, d);
        mStored             = sequence != 0;
        if (mStored) {
            gThread.parent = sequence;
        }
    } catch (...) {}
}

OfflineRenderTraceScope::~OfflineRenderTraceScope() noexcept {
    if (mEpoch == 0 || gThread.epoch != mEpoch || gTrace.activeEpoch.load(std::memory_order_acquire) != mEpoch) {
        return;
    }
    try {
        std::lock_guard recordLock(gTrace.recordMutex);
        if (gTrace.activeEpoch.load(std::memory_order_acquire) == mEpoch) {
            appendEvent(
                mExit,
                mObject,
                mRelated,
                mResult,
                mB,
                mC,
                mD,
                mStored ? TraceStorage::Keep : TraceStorage::Suppress
            );
        }
    } catch (...) {}
    if (gThread.epoch == mEpoch) {
        gThread.parent = mPreviousParent;
    }
}

void OfflineRenderTraceScope::result(uint64_t value) noexcept { mResult = value; }

} // namespace playback::exporting
