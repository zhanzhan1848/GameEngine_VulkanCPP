// Opt-in main-game-viewport capture. Does not change Lumen's tracing budgets.
#include "Modules/ModuleManager.h"
#include "SceneViewExtension.h"
#include "SceneView.h"
#include "SceneInterface.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "ContentStreaming.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "PipelineStateCache.h"
#if WITH_EDITOR
#include "AssetCompilingManager.h"
#endif
#include "UnrealClient.h"
#include "RenderGraphBuilder.h"
#include "DumpGPU.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Misc/CoreDelegates.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogLumenCompare, Log, All);

class FComparisonView final : public FSceneViewExtensionBase
{
public:
    FComparisonView(const FAutoRegister& AutoRegister, const FString& InOutput)
        : FSceneViewExtensionBase(AutoRegister), Output(InOutput) {}
    virtual void SetupViewFamily(FSceneViewFamily& Family) override {
        if (!Prepared) {
            Family.EngineShowFlags.SetGlobalIllumination(false);
            Family.EngineShowFlags.SetLumenGlobalIllumination(false);
            Family.EngineShowFlags.SetLumenReflections(false);
        }
    }
    virtual void SetupView(FSceneViewFamily&, FSceneView& View) override {
        if (Prepared && Ordinal == 0) View.bCameraCut = true;
    }
    bool Prepare(UWorld* World) {
        if (Prepared) return true;
        if (!GEngine || !GEngine->IsInitialized() || !World) return false;
        for (const auto& Pair : {TPair<const TCHAR*, const TCHAR*>(TEXT("r.ScreenPercentage"), TEXT("100")),
                                TPair<const TCHAR*, const TCHAR*>(TEXT("r.DynamicRes.OperationMode"), TEXT("0")),
                                TPair<const TCHAR*, const TCHAR*>(TEXT("r.Lumen.HardwareRayTracing"), TEXT("0"))})
            if (auto* C = IConsoleManager::Get().FindConsoleVariable(Pair.Key)) C->Set(Pair.Value, ECVF_SetByCode);
        FlushAsyncLoading();
#if WITH_EDITOR
        FAssetCompilingManager::Get().FinishAllCompilation();
        const int32 PendingAssets = FAssetCompilingManager::Get().GetNumRemainingAssets();
#else
        const int32 PendingAssets = 0;
#endif
        if (GShaderCompilingManager) GShaderCompilingManager->FinishAllCompilation();
        FlushRenderingCommands();
        PipelineStateCache::WaitForAllTasks();
        int32 Meshes = 0;
        int32 PendingPSOs = 0;
        for (TActorIterator<AStaticMeshActor> It(World); It; ++It) {
            auto* Component = It->GetStaticMeshComponent();
            if (!Component || !Component->GetStaticMesh()) continue;
            if (Component->GetStaticMesh()->GetPathName().StartsWith(TEXT("/Game/Sponza/Meshes/"))) ++Meshes;
            if (Component->CheckPSOPrecachingAndBoostPriority() || Component->ShouldRenderProxyFallbackToDefaultMaterial()) ++PendingPSOs;
            Component->PrestreamTextures(120.f, false);
        }
        if (Meshes != 393 || PendingAssets != 0 || PendingPSOs != 0) return false;
        auto& Streaming = IStreamingManager::Get();
        Streaming.AddViewInformation(FVector(-1250., -70., 170.), 2560.f, 2560.f * .7330645f, 1.f, true);
        const int32 PendingStreaming = Streaming.StreamAllResources(120.f);
        const int32 PendingIO = Streaming.BlockTillAllRequestsFinished(120.f, true);
        FlushRenderingCommands();
        if (PendingStreaming != 0 || PendingIO != 0) return false;
        // Replace proxies created while material shaders/PSOs were unavailable.
        for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
            It->GetStaticMeshComponent()->RecreateRenderState_Concurrent();
        FlushRenderingCommands();
        auto* Player = World->GetFirstPlayerController();
        if (!Player) return false;
        // The native command resets FSceneViewState and releases its persistent
        // rendering resources. No Lumen view is measured before this boundary.
        Player->ConsoleCommand(TEXT("r.ResetViewState"), false);
        if (auto* Reset = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LumenScene.SurfaceCache.Reset")))
            Reset->Set(1, ECVF_SetByCode);
        FlushRenderingCommands();
        Prepared = true;
        FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"verified\":true,\"instances\":%d,\"expected_instances\":393,\"pending_asset_compiles\":%d,\"pending_streaming\":%d,\"pending_io\":%d,\"startup_views\":%d,\"startup_gi_disabled\":true,\"view_state_reset_requested\":true,\"surface_cache_reset_requested\":true}"), Meshes, PendingAssets, PendingStreaming, PendingIO, StartupViews), *(Output / TEXT("scene-readiness.json")));
        return true;
    }
    virtual ~FComparisonView() override {
        if (Viewport.IsValid()) Viewport->OnEndDraw().Remove(EndDrawHandle);
    }
    void EndDraw() {
#if WITH_DUMPGPU
        if (OwnedDumpActive && UE::RenderCore::DumpGPU::IsDumpingFrame()) {
            UE::RenderCore::DumpGPU::TickEndFrame();
            FFileHelper::SaveStringToFile(FString::Printf(TEXT("{\"ordinal\":%d,\"main_families_in_scope\":%d,\"closed_at\":\"GameViewport.OnEndDraw\"}"), Ordinal, FamiliesInScope),
                *(Output / FString::Printf(TEXT("frame-%d-boundary.json"), Ordinal)));
            OwnedDumpActive = false;
        }
#endif
    }

    virtual void SetupViewPoint(APlayerController* Player, FMinimalViewInfo& Info) override
    {
        if (!Player || !Player->GetWorld()->IsGameWorld()) return;
        for (TActorIterator<ACameraActor> It(Player->GetWorld()); It; ++It)
            if (It->GetName().Contains(TEXT("CameraActor_0"))) {
                It->GetCameraComponent()->GetCameraView(0.f, Info);
                // Primal's 75 degrees is vertical; Unreal's Camera FOV is horizontal.
                Info.FOV = FMath::RadiansToDegrees(2.f * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(75.f) * .5f) * (2560.f / 1440.f)));
                Info.bConstrainAspectRatio = false;
                break;
            }
    }

    virtual void BeginRenderViewFamily(FSceneViewFamily& Family) override
    {
        if (!GEngine || !GEngine->GameViewport || Family.RenderTarget != GEngine->GameViewport->Viewport ||
            Family.Views.Num() != 1 || !Family.Scene || !Family.Scene->GetWorld()->IsGameWorld()) return;
        if (!Prepared) { ++StartupViews; return; }
        if (!Viewport.IsValid()) {
            Viewport = GEngine->GameViewport;
            EndDrawHandle = Viewport->OnEndDraw().AddRaw(this, &FComparisonView::EndDraw);
        }
        if (OwnedDumpActive) ++FamiliesInScope;
        // Ordinal is based on actual main-view-family submissions, never Python/Slate ticks.
        ++Ordinal;
        if (Ordinal == 1) NativeOrigin = Family.FrameNumber;
        if (!Frames.Contains(Ordinal)) return;
#if WITH_DUMPGPU
        if (UE::RenderCore::DumpGPU::IsDumpingFrame())
        {
            UE_LOG(LogLumenCompare, Error, TEXT("Capture collision at ordinal %d"), Ordinal);
            return;
        }
        if (auto* C = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DumpGPU.Directory")))
            C->Set(*(Output / FString::Printf(TEXT("frame-%d"), Ordinal)), ECVF_SetByCode);
        const FString Directory = FRDGBuilder::BeginResourceDump(TEXT("-oneframe"));
        // Begin here, before this family's RDG is built, rather than arming a
        // console command that would capture the following engine tick.
        UE::RenderCore::DumpGPU::TickEndFrame();
        OwnedDumpActive = true;
        FamiliesInScope = 1;
        FScreenshotRequest::RequestScreenshot(Output / FString::Printf(TEXT("frame-%d.png"), Ordinal), false, false);
        const FSceneView& View = *Family.Views[0];
        TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
        Record->SetNumberField(TEXT("ordinal"), Ordinal);
        Record->SetNumberField(TEXT("engine_frame_counter"), double(Family.FrameCounter));
        Record->SetNumberField(TEXT("native_frame_number"), Family.FrameNumber);
        Record->SetNumberField(TEXT("native_frame_origin"), NativeOrigin);
        Record->SetStringField(TEXT("dump_directory"), Directory);
        Record->SetStringField(TEXT("scope"), TEXT("main_game_viewport_single_view"));
        Record->SetStringField(TEXT("frame_origin"), TEXT("first GI main view after resource barrier and history reset"));
        Record->SetStringField(TEXT("world"), Family.Scene->GetWorld()->GetPathName());
        Record->SetStringField(TEXT("view_origin_cm"), View.ViewMatrices.GetViewOrigin().ToString());
        Record->SetStringField(TEXT("view_state_identity"), FString::Printf(TEXT("%p"), View.State));
        const auto MatrixValues = [](const FMatrix& Matrix) {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (int32 Row = 0; Row < 4; ++Row) for (int32 Col = 0; Col < 4; ++Col)
                Values.Add(MakeShared<FJsonValueNumber>(Matrix.M[Row][Col]));
            return Values;
        };
        Record->SetArrayField(TEXT("view_matrix"), MatrixValues(View.ViewMatrices.GetViewMatrix()));
        Record->SetArrayField(TEXT("projection_matrix"), MatrixValues(View.ViewMatrices.GetProjectionMatrix()));
        Record->SetNumberField(TEXT("width"), View.UnscaledViewRect.Width());
        Record->SetNumberField(TEXT("height"), View.UnscaledViewRect.Height());
        Record->SetBoolField(TEXT("capture_armed"), !Directory.IsEmpty());
        Record->SetBoolField(TEXT("complete"), false); // Native dump status + binaries must be validated offline.
        FString Json;
        FJsonSerializer::Serialize(Record, TJsonWriterFactory<>::Create(&Json));
        FFileHelper::SaveStringToFile(Json, *(Output / FString::Printf(TEXT("frame-%d-request.json"), Ordinal)));
        UE_LOG(LogLumenCompare, Display, TEXT("ordinal=%d dump=%s engine_counter=%llu"), Ordinal, *Directory, Family.FrameCounter);
#else
        UE_LOG(LogLumenCompare, Error, TEXT("WITH_DUMPGPU is disabled"));
#endif
    }
    int32 Ordinal = 0;
    int32 StartupViews = 0;
    uint32 NativeOrigin = 0;
    bool Prepared = false;
    const TSet<int32> Frames{1, 5, 35, 75, 150, 360};
    FString Output;
    bool OwnedDumpActive = false;
    int32 FamiliesInScope = 0;
    TWeakObjectPtr<UGameViewportClient> Viewport;
    FDelegateHandle EndDrawHandle;
};

class FLumenCompareCaptureModule final : public IModuleInterface
{
    TSharedPtr<FComparisonView, ESPMode::ThreadSafe> Extension;
    FDelegateHandle BeginHandle;
public:
    virtual void StartupModule() override
    {
        FString Output;
        if (!FParse::Value(FCommandLine::Get(), TEXT("LumenCompareOutput="), Output)) return;
        Output = FPaths::ConvertRelativePathToFull(Output);
        IFileManager::Get().MakeDirectory(*Output, true);
        const auto Set = [](const TCHAR* Name, const TCHAR* Value) {
            if (auto* C = IConsoleManager::Get().FindConsoleVariable(Name)) C->Set(Value, ECVF_SetByCode);
        };
        Set(TEXT("r.DumpGPU.Directory"), *Output);
        Set(TEXT("r.DumpGPU.Explore"), TEXT("0"));
        Set(TEXT("r.DumpGPU.Upload"), TEXT("0"));
        Set(TEXT("r.DumpGPU.Stream"), TEXT("0"));
        Set(TEXT("r.DumpGPU.Root"), TEXT("*Lumen*"));
        Set(TEXT("r.DumpGPU.FrameCount"), TEXT("1"));
        Set(TEXT("r.DumpGPU.FrameDelay"), TEXT("0"));
        Set(TEXT("r.DumpGPU.Delay"), TEXT("0"));
        Set(TEXT("r.DumpGPU.CameraCut"), TEXT("0"));
        Extension = FSceneViewExtensions::NewExtension<FComparisonView>(Output);
        BeginHandle = FCoreDelegates::OnBeginFrame.AddLambda([this] {
            if (Extension.IsValid() && Extension->Ordinal >= 360) {
                FPlatformMisc::RequestExit(false); return;
            }
            if (!GEngine || !GEngine->GameViewport) return;
            UWorld* World = GEngine->GameViewport->GetWorld();
            if (!World) return;
            if (Extension.IsValid() && !Extension->Prepare(World)) return;
            if (auto* PC = World->GetFirstPlayerController())
                for (TActorIterator<ACameraActor> It(World); It; ++It)
                    if (It->GetName().Contains(TEXT("CameraActor_0"))) { PC->SetViewTarget(*It); break; }
        });
        UE_LOG(LogLumenCompare, Display, TEXT("Armed 1,5,35,75,150,360 in %s"), *Output);
    }
    virtual void ShutdownModule() override
    {
        FCoreDelegates::OnBeginFrame.Remove(BeginHandle);
        Extension.Reset();
    }
};
IMPLEMENT_MODULE(FLumenCompareCaptureModule, LumenCompareCapture)
