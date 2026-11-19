#include "FrameMath.h"
#include "PresentRate.h"
#include "LiveTiming.h"
#include "RenderWorkQueue.h"
#include "FsrProvider.h"
#include "FgErrorBurst.h"
#include "FgRetry.h"
#include "MipBias.h"
#include "FgViewportMath.h"
#include "ExposurePolicy.h"
#include "PresentHookSlots.h"
#include "PresentHookRetry.h"
#include "FgDropNotice.h"
#include "TaaJitter.h"
#include "Preset.h"
#include "TestHalfFloat.h"
#include "PrimaryCamera.h"
#include <cassert>
#include <mutex>
#include <thread>
static_assert(frame_math::SceneCamera(true,true,true,false,false));
static_assert(!frame_math::SceneCamera(true,true,false,false,false));
static_assert(!frame_math::SceneCamera(true,false,true,false,false));
static_assert(!frame_math::SceneCamera(true,true,true,true,false));
static_assert(!frame_math::SceneCamera(true,true,true,false,true));
static_assert(frame_math::SupportedTarget(2,1,false));
static_assert(!frame_math::SupportedTarget(4,1,false));
static_assert(!frame_math::SupportedTarget(2,4,false));
static_assert(!frame_math::SupportedTarget(2,1,true));
static_assert(frame_math::UpscaleSource(1440,2560,1440,2560,2160,3840));
static_assert(!frame_math::UpscaleSource(2160,3840,1440,2560,2160,3840));
static_assert(!frame_math::UpscaleSource(1441,2560,1440,2560,2160,3840));
static_assert(!frame_math::RetryCamera(4,4,29,30));
static_assert(frame_math::RetryCamera(4,4,30,30));
static_assert(frame_math::RetryCamera(5,4,29,30));
static_assert(frame_math::EarlyNativeAA(true,false,1.0f));
static_assert(!frame_math::EarlyNativeAA(false,false,1.0f));
static_assert(!frame_math::EarlyNativeAA(true,true,1.0f));
static_assert(!frame_math::EarlyNativeAA(true,false,1.5f));
static_assert(frame_math::FsrInputIsHdr(true,false,true));
static_assert(frame_math::FsrInputIsHdr(true,true,false));
static_assert(!frame_math::FsrInputIsHdr(true,false,false));
static_assert(!frame_math::FsrInputIsHdr(false,false,true));
int main() {
    {
        LiveTimingTracker timing;
        constexpr uint64_t hz=1000000;
        timing.ObservePresent(1000000,hz);
        timing.ObserveInput(1002000);
        timing.ObservePresent(1010000,hz);
        const auto first=timing.Snapshot(1010000,hz);
        assert(first.present.samples==1 && first.present.medianMs==10);
        assert(first.input.samples==1 && first.input.medianMs==8);
        timing.ObservePresent(1020000,hz);
        assert(timing.Snapshot(1020000,hz).input.samples==1); // one event, one Present
        timing.ObserveInput(1022000);
        timing.ObserveInput(1025000); // the latest consumed input event wins
        timing.ObservePresent(1030000,hz);
        const auto next=timing.Snapshot(1030000,hz);
        assert(next.present.samples==3 && next.present.p99Ms==10 && next.present.stddevMs==0);
        assert(next.input.samples==2 && next.input.medianMs==5 && next.input.p95Ms==8);
        timing.ObserveInput(1032000);
        timing.ObservePresent(2100000,hz); // a long hitch is not a pacing sample or stale input sample
        assert(timing.Snapshot(2100000,hz).present.samples==3);
        assert(timing.Snapshot(2100000,hz).input.samples==2);
        assert(timing.Snapshot(13000000,hz).present.samples==0);
        assert(timing.Snapshot(13000000,hz).input.samples==0);
        timing.Reset();
        assert(timing.Snapshot(1030000,hz).present.samples==0);
    }
    {
        LiveTimingTracker timing;
        constexpr uint64_t hz = 1000;
        timing.ObserveInput(130);
        timing.ObservePresent(120, hz); // This Present entered before the input arrived.
        assert(timing.Snapshot(130, hz).input.samples == 0);
        timing.ObservePresent(150, hz);
        const auto input = timing.Snapshot(150, hz).input;
        assert(input.samples == 1 && input.medianMs == 20);
    }
    {
        TimingSampleWindow<3> window;
        for(uint64_t i=1;i<=4;++i) window.Add(static_cast<double>(i),i*1000);
        const auto samples=window.Snapshot(4000,1000);
        assert(samples.samples==3 && samples.medianMs==3 && samples.p95Ms==4 && samples.p99Ms==4);
        assert(window.Snapshot(15000,1000).samples==0); // samples expire after ten seconds
        TimingSampleWindow<100> percentiles;
        for(uint64_t i=1;i<=100;++i) percentiles.Add(static_cast<double>(i),1000+i);
        const auto distribution=percentiles.Snapshot(1200,1000);
        assert(distribution.samples==100 && distribution.medianMs==50);
        assert(distribution.p95Ms==95 && distribution.p99Ms==99);
    }
    PresentRate fps;
    fps.Sample(100,{0,0});
    assert(!fps.ready);
    fps.Sample(1100,{60,60});
    assert(fps.ready && fps.realFps==60 && fps.totalFps==120);
    fps.Sample(2100,{120,60});
    assert(fps.ready && fps.realFps==60 && fps.totalFps==60);
    fps.Sample(6100,{300,60});
    assert(!fps.ready);
    ResourceLifetime oldTextures, newTextures;
    oldTextures.Begin();
    oldTextures.Begin();
    newTextures.Begin();
    newTextures.Complete();
    assert(!newTextures.InUse() && oldTextures.InUse());
    oldTextures.Complete();
    assert(oldTextures.InUse());
    oldTextures.Complete();
    assert(!oldTextures.InUse());
    {
        struct Packet { std::shared_ptr<int> nativeTexture; };
        RenderWorkQueue<Packet,32> pending;
        auto retained = std::make_shared<int>(1);
        const auto skipped = pending.Begin(newTextures);
        assert(pending.Attach(skipped,std::make_unique<Packet>(Packet{retained})));
        pending.Complete(skipped);
        assert(!newTextures.InUse() && !pending.Take(skipped) && retained.use_count()==1);
        const auto rendered = pending.Begin(oldTextures);
        assert(pending.Attach(rendered,std::make_unique<Packet>(Packet{retained})));
        auto inflight = pending.Take(rendered);
        assert(inflight && !pending.Take(rendered));
        const auto abandoned = pending.Begin(oldTextures);
        assert(pending.Attach(abandoned,std::make_unique<Packet>(Packet{retained})));
        for(int i=2;i<32;++i) {
            const auto id = pending.Begin(newTextures);
            assert(id && pending.Attach(id,std::make_unique<Packet>(Packet{retained})));
        }
        assert(pending.Pending()==32 && pending.Begin(newTextures)==0 && retained.use_count()==33);
        pending.Complete(rendered);
        const auto later = pending.Begin(newTextures);
        assert(later && later!=rendered);
        pending.Complete(rendered); // Replaying an old Unity marker cannot release the reused slot.
        assert(pending.Pending()==32 && oldTextures.InUse() && newTextures.InUse());
        pending.CancelPackets();
        assert(pending.Begin(newTextures)==0 && !pending.Take(abandoned) && retained.use_count()==2);
        assert(pending.Pending()==32 && oldTextures.InUse());
        pending.Complete(abandoned);
        assert(!oldTextures.InUse() && newTextures.InUse()); // A late marker can still release the roots.
        inflight.reset();
        assert(retained.use_count()==1);
    }
    assert(ParseFsrProviderVersion("4.1.2").valid);
    assert(ParseFsrProviderVersion("4.1").valid);
    assert(!ParseFsrProviderVersion("4.").valid);
    assert(!ParseFsrProviderVersion("4.1.bad").valid);
    assert(!ParseFsrProviderVersion("4.1000000000.1").valid);
    assert(NewerFsr4Provider(ParseFsrProviderVersion("4.10.0"),ParseFsrProviderVersion("4.9.99")));
    assert(!NewerFsr4Provider(ParseFsrProviderVersion("3.99.99"),{}));
    assert(!NewerFsr4Provider(ParseFsrProviderVersion("4.1.1"),ParseFsrProviderVersion("4.1.2")));
    FgRetry retry;
    retry.ObserveScene(1920,1080,1280,720);
    assert(retry.Ready(0));
    for (uint32_t n=1; n<=FgRetry::MaxFailures; ++n) {
        uint32_t delay=retry.Failed(1000, false);
        assert(delay==(n==FgRetry::MaxFailures ? 0u : (500u << (n-1))));
        assert(!retry.Ready(1000+delay-1));
        if (n<FgRetry::MaxFailures) assert(retry.Ready(1000+delay));
    }
    assert(!retry.Ready(1000000)); // bounded retries
    retry.ObserveBackbuffer(1920,1080);
    assert(!retry.Ready(1000000)); // first observation only sets the baseline
    retry.ObserveBackbuffer(1600,900); // resize re-enables the FG path
    assert(retry.Ready(1000000));
    retry.Failed(1000000, true); // unsupported provider is permanent for the device
    retry.ObserveScene(1080,1920,720,1280);
    assert(!retry.Ready(2000000));
    retry.NewDevice();
    assert(retry.Ready(0));
    // A successful D3D11 shared-texture allocation does not mean FG dispatch
    // succeeded. Simulate Configure/Prepare/Dispatch errors in sequence.
    FgRetry providerErrors;
    providerErrors.ObserveScene(1920,1080,1280,720);
    providerErrors.ObserveBackbuffer(1920,1080);
    uint64_t retryTime = 1000;
    for(uint32_t failure=1;failure<=FgRetry::MaxFailures;++failure) {
        assert(providerErrors.Ready(retryTime));
        const auto delay = providerErrors.Failed(retryTime,false);
        if(failure<FgRetry::MaxFailures) {
            assert(delay==500u<<(failure-1));
            assert(!providerErrors.Ready(retryTime+delay-1));
            retryTime+=delay;
        } else {
            assert(delay==0);
            assert(!providerErrors.Ready(retryTime+60000));
        }
    }
    providerErrors.ObserveBackbuffer(1920,1080);
    assert(!providerErrors.Ready(retryTime+60000));
    providerErrors.ObserveBackbuffer(1600,900);
    assert(providerErrors.Ready(retryTime));
    providerErrors.Failed(retryTime,false);
    assert(!providerErrors.Ready(retryTime));
    providerErrors.Recovered(); // Only a successful FG dispatch resets the budget.
    assert(providerErrors.Ready(retryTime));
    float prevForward[3]{0,0,1};
    float smoothForward[3]{0.173648f,0,0.984808f}; // 10-degree pan
    float jumpForward[3]{0.866025f,0,0.5f}; // 60-degree cut
    float reverseForward[3]{0,0,-1};
    float zeroForward[3]{};
    assert(!frame_math::RotationJump(prevForward,prevForward));
    assert(!frame_math::RotationJump(smoothForward,prevForward));
    assert(frame_math::RotationJump(jumpForward,prevForward));
    assert(frame_math::RotationJump(reverseForward,prevForward));
    assert(frame_math::RotationJump(prevForward,zeroForward));
    FgErrorBurst fgErrors;
    assert(fgErrors.Record(1, 7));
    assert(fgErrors.Record(1, 7));
    assert(fgErrors.Record(1, 7));
    assert(!fgErrors.Record(1, 7));
    assert(!fgErrors.Record(1, 7));
    assert(fgErrors.Record(2, 7)); // new stage, new burst
    assert(fgErrors.Record(2, 8)); // new code, new burst
    fgErrors.Reset();
    assert(fgErrors.Record(2, 8)); // successful frame restored logging
    assert(std::abs(RecommendedFsrMipBias(1.5f)+1.5849625f)<0.00001f);
    assert(std::abs(RecommendedFsrMipBias(1.7f)+1.7655347f)<0.00001f);
    assert(std::abs(RecommendedFsrMipBias(2.0f)+2.0f)<0.00001f);
    assert(RecommendedFsrMipBias(1.0f)==0.0f);
    assert(TestFloatToHalf(1.0f)==0x3c00u);
    assert(TestFloatToHalf(-1.0f)==0xbc00u);
    assert(TestFloatToHalf(0.5f)==0x3800u);
    assert(TestFloatToHalf(0.0f)==0);
    assert(frame_math::MotionVectorYScale(720,-1)==-720.0f);
    assert(frame_math::MotionVectorYScale(720,1)==720.0f);
    assert(std::abs(frame_math::Jitter(0.5f/640,1280,1)-0.5f)<0.00001f);
    assert(std::abs(frame_math::Jitter(0.5f/640,1280,-1)+0.5f)<0.00001f);
    auto portraitOnLandscape = FgAspectFit(1920,1080,1080,1920);
    assert(FgViewportWithinBackbuffer({400,100,1080,608},1920,1080,1920,1080));
    assert(!FgViewportWithinBackbuffer({1900,100,1080,608},1920,1080,1920,1080));
    assert(!FgViewportWithinBackbuffer({400,100,1080,608},1920,1080,1080,1920));
    assert(portraitOnLandscape.x==656 && portraitOnLandscape.y==0);
    assert(portraitOnLandscape.width==608 && portraitOnLandscape.height==1080);
    auto landscapeOnPortrait = FgAspectFit(1080,1920,1920,1080);
    assert(landscapeOnPortrait.x==0 && landscapeOnPortrait.y==656);
    assert(landscapeOnPortrait.width==1080 && landscapeOnPortrait.height==608);
    auto letterbox = FgAspectFit(1920,1080,3840,1634);
    assert(letterbox.x==0 && letterbox.y==131 && letterbox.width==1920 && letterbox.height==817);
    auto same = FgAspectFit(1920,1080,1280,720);
    assert(same.x==0 && same.y==0 && same.width==1920 && same.height==1080);
    assert(FgAspectFit(1920,1080,0,1080).width==0);
    assert(!HasExternalExposure(true,0.0f));
    assert(!HasExternalExposure(false,1.25f));
    assert(HasExternalExposure(true,1.25f));
    assert(!HasExternalExposure(true,INFINITY));
    void* first[23]{}; void* second[23]{};
    PresentHookSlots hooked;
    int original1=1, original2=2;
    assert(hooked.Remember(&first[8], &original1));
    assert(!hooked.Remember(&first[8], &original2));
    assert(hooked.Remember(&second[8], &original2));
    assert(hooked.Find(&first[8]) == &original1);
    assert(hooked.Find(&second[8]) == &original2);
    assert(hooked.Find(&second[22]) == nullptr);
    PresentHookRetry presentProbe;
    assert(presentProbe.Allow(1000));
    assert(!presentProbe.Allow(1001));
    assert(!presentProbe.Allow(5999));
    assert(presentProbe.Allow(6000));
    assert(!presentProbe.Allow(6001));
    assert(presentProbe.Allow(100)); // permit re-probe if the monotonic clock resets
    FgDropNotice dropNotice;
    int gameChain=0, gameChain1=0, otherChain=0;
    dropNotice.SetTarget(&gameChain,&gameChain1);
    {
        std::mutex renderLock;
        std::lock_guard<std::mutex> rendering(renderLock);
        std::thread present([&] {
            std::unique_lock<std::mutex> lock(renderLock,std::try_to_lock);
            assert(!lock.owns_lock());
            dropNotice.Mark(&otherChain);
            assert(!dropNotice.Consume());

            dropNotice.Mark(&gameChain);
            dropNotice.Mark(&gameChain1);
        });
        present.join();
        assert(dropNotice.Consume());
        assert(!dropNotice.Consume());
    }
    dropNotice.Mark(&gameChain1);
    assert(dropNotice.Consume());
    dropNotice.SetTarget(nullptr);
    dropNotice.Mark(&gameChain);
    assert(!dropNotice.Consume());
    PrimaryCamera camera;
    assert(camera.Allow(11,10));
    assert(!camera.Allow(22,10));
    assert(camera.Allow(11,11));
    assert(!camera.Allow(22,11));
    assert(!camera.Allow(22,12)); // retain the first camera across a one-frame gap
    assert(camera.Allow(22,13)); // first camera disappeared, allow replacement
    assert(!camera.Allow(11,13));
    assert(camera.Allow(33,1));
    assert(!camera.Allow(22,1));
    assert(camera.Allow(33,2));
    camera.Reset();
    assert(camera.Allow(11,14));
    assert(FsrRecommendedJitterPhases(1.5f)==18);
    assert(FsrRecommendedJitterPhases(1.0f)==8);
    assert(FsrRecommendedJitterPhases(1.7f)==23);
    assert(FsrRecommendedJitterPhases(2.0f)==32);
    assert(FsrRecommendedJitterPhases(3.0f)==72);
    assert(SelectJitterPhaseCount(0,1.0f)==1024);
    assert(SelectJitterPhaseCount(1,1.0f)==8);
    assert(SelectJitterPhaseCount(2,1.0f)==8);
    assert(SelectJitterPhaseCount(2,1.5f)==1024);
    assert(SelectJitterPhaseCount(1,1.5f)==18);
    assert(SelectJitterPhaseCount(3,1.0f)==1024);
    auto nativePhaseA=RepairTaaJitter(0,0,1920,1080,1920,1080,2,0,1,8);
    auto nativePhaseB=RepairTaaJitter(0,0,1920,1080,1920,1080,10,0,1,8);
    assert(nativePhaseA.x==nativePhaseB.x && nativePhaseA.y==nativePhaseB.y);
    auto phaseA=RepairTaaJitter(1,1,1920,1080,1280,720,0,-1,1,18);
    auto phaseB=RepairTaaJitter(0,0,1920,1080,1280,720,18,-1,1,18);
    assert(phaseA.x==phaseB.x && phaseA.y==phaseB.y);
    auto generated=RepairTaaJitter(0,0,1920,1080,1280,720,1,0,1.0f);
    assert(std::isfinite(generated.x) && std::isfinite(generated.y));
    assert(std::fabs(generated.x)+std::fabs(generated.y)>0);
    auto generatedDefault=RepairTaaJitter(0,0,1920,1080,1280,720,1,0,0.0f);
    assert(generatedDefault.x==generated.x && generatedDefault.y==generated.y);
    auto generatedNext=RepairTaaJitter(0,0,1920,1080,1280,720,2,0,1.0f);
    assert(generated.x!=generatedNext.x || generated.y!=generatedNext.y);
    auto rescaled=RepairTaaJitter(0.25f/1920,0.5f/1080,1920,1080,1280,720,1,0,1.0f);
    assert(std::fabs(rescaled.x-0.25f/1280)<0.000001f);
    assert(std::fabs(rescaled.y-0.5f/720)<0.000001f);
    assert(std::fabs(TaaHalton(1,2)-0.5f)<0.000001f);
    assert(PresetRatio(L"NativeAA")==1.0f);
    assert(PresetRatio(L"quality")==1.5f);
    assert(PresetRatio(L"Balanced")==1.7f);
    assert(PresetRatio(L"Performance")==2.0f);
    assert(PresetRatio(L"UltraPerformance")==3.0f);
    assert(PresetRatio(L"unknown")==0.0f);
    assert(frame_math::RenderSize(1920,1.5f)==1280);
    assert(frame_math::RenderSize(1080,1.5f)==720);
    assert(frame_math::RenderSize(2160,1.5f)==1440);
    assert(frame_math::RenderSize(3840,1.5f)==2560);
    assert(frame_math::RenderSize(1,3)==1);
    assert(frame_math::NativeSurfaceDimensions(1920,1080,1920,1080,1920,1080));
    assert(!frame_math::NativeSurfaceDimensions(1920,1080,1920,1079,1920,1080));
    assert(!frame_math::NativeSurfaceDimensions(2560,1440,1920,1080,1920,1080));
    assert(!frame_math::NativeSurfaceDimensions(1920,1080,1920,1080,0,1080));
    bool rejected=false;
    try { frame_math::RenderSize(0,1.5f); } catch (...) { rejected=true; }
    assert(rejected);
    assert(std::abs(frame_math::Jitter(0.5f/640,1280)+0.5f)<0.00001f);
    float a[3]{}, b[3]{6,0,0};
    assert(!frame_math::Reset(1,1,2,1,45,45,a,a));
    assert(frame_math::Reset(2,1,2,1,45,45,a,a));
    assert(frame_math::Reset(1,1,4,1,45,45,a,a));
    assert(frame_math::Reset(1,1,2,1,45,45,a,b));
    assert(frame_math::Reset(1,1,2,1,51,45,a,a));
}
