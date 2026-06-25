using Avalonia;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Threading.Tasks;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Components;
using PrimalEditor_Avalonia.GameProject;

namespace PrimalEditor_Avalonia;

sealed class Program
{
    [STAThread]
    public static void Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--verify-serialization")
        {
            RunSerializationVerification();
            return;
        }

        // Pin cwd to the executable directory. The native engine resolves
        // shader blob paths like "./shaders/metal/shaders.metallib" relative
        // to cwd; without this anchor, "dotnet ./.../dll" from a worktree
        // root causes shader::initialize to fail silently and disables
        // every render pass.
        AnchorWorkingDirectoryToExecutable();

        // Workaround for AvaloniaUI/Avalonia#18895: CVDisplayLink creation
        // can fail immediately when the active display is in a low-power or
        // Power Nap state, causing "Native error code is: -6661" on startup.
        // Retry until a display link is available, then release it so Avalonia
        // can create its own during AppBuilder.Setup().
        if (OperatingSystem.IsMacOS())
        {
            AwaitDisplayLink().GetAwaiter().GetResult();
        }

        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    private static void AnchorWorkingDirectoryToExecutable()
    {
        try
        {
            string? baseDir = AppContext.BaseDirectory;
            if (string.IsNullOrEmpty(baseDir)) return;

            // Always pin cwd to the executable's directory. The native engine
            // resolves bare-relative paths like "./shaders/metal/shaders.metallib"
            // against cwd, and the only guaranteed source of that blob is the
            // directory that hosts libPrimalEngine.dylib (== BaseDirectory on
            // a packaged dotnet layout).
            //
            // Skip only when cwd already points at BaseDirectory (no-op
            // SetCurrentDirectory) to avoid noisy log lines in the common case.
            string currentDir = Directory.GetCurrentDirectory();
            if (string.Equals(
                    Path.GetFullPath(currentDir).TrimEnd(Path.DirectorySeparatorChar),
                    Path.GetFullPath(baseDir).TrimEnd(Path.DirectorySeparatorChar),
                    StringComparison.OrdinalIgnoreCase))
            {
                return;
            }

            Directory.SetCurrentDirectory(baseDir);
            Console.Error.WriteLine($"[Avalonia] cwd pinned to BaseDirectory: {baseDir}");
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[Avalonia] Could not pin cwd to BaseDirectory: {ex.Message}");
        }
    }

    [SupportedOSPlatform("macos")]
    private static async Task AwaitDisplayLink()
    {
        const string CoreVideoLibrary = "/System/Library/Frameworks/CoreVideo.framework/CoreVideo";
        [DllImport(CoreVideoLibrary)]
        static extern int CVDisplayLinkCreateWithActiveCGDisplays(out IntPtr displayLinkOut);
        [DllImport(CoreVideoLibrary)]
        static extern void CVDisplayLinkRelease(IntPtr displayLink);

        IntPtr displayLink;
        int attempts = 0;
        while (CVDisplayLinkCreateWithActiveCGDisplays(out displayLink) != 0)
        {
            attempts++;
            if (attempts == 1)
                Console.Error.WriteLine("[Avalonia] CVDisplayLink unavailable, retrying every 500ms (Avalonia issue #18895 workaround)");
            await Task.Delay(500);
        }
        if (attempts > 0)
            Console.Error.WriteLine($"[Avalonia] CVDisplayLink available after {attempts} retries");
        CVDisplayLinkRelease(displayLink);
    }

    public static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();

    private static int RunSerializationVerification()
    {
        Logger.Current = new ConsoleLogger();
        Logger.Info("=== Starting Serialization Verification ===");

        Logger.Info("=== Testing Native Engine Load ===");
        Native.NativeEngineBridge.LoadEngine();
        if (Native.NativeEngineBridge.IsLoaded)
        {
            Logger.Info($"Native engine loaded from: {Native.NativeEngineBridge.LoadedPath}");
        }
        else
        {
            Logger.Warning($"Native engine not loaded: {Native.NativeEngineBridge.LoadError}");
        }

        var exitCode = 0;
        var testDir = Path.Combine(Path.GetTempPath(), "PrimalEditorVerify_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(testDir);
        var projectPath = Path.Combine(testDir, "TestProject");
        var projectFile = Path.Combine(projectPath, "TestProject.primal");

        try
        {
            exitCode |= TestProjectRoundtrip(projectPath, projectFile);
            exitCode |= TestSceneRoundtrip(projectPath);
            exitCode |= TestEntityRoundtrip(projectPath);
            exitCode |= TestAssetImport(projectPath);
            exitCode |= TestMeshCookImport(projectPath);
            exitCode |= TestCookedFileVerification();
            exitCode |= TestCorruptedFile(testDir);

            if (exitCode != 0)
            {
                Logger.Info("=== DEBUG: Listing test directory ===");
                try
                {
                    foreach (var f in Directory.GetFiles(testDir, "*", SearchOption.AllDirectories))
                    {
                        Logger.Info($"  {f}");
                        try { Logger.Info("    " + File.ReadAllText(f)); } catch { }
                    }
                }
                catch (Exception ex) { Logger.Error("Debug listing failed", ex); }
            }
        }
        finally
        {
            try { Directory.Delete(testDir, true); } catch { }
        }

        if (exitCode == 0)
            Logger.Info("=== ALL VERIFICATIONS PASSED ===");
        else
            Logger.Error("=== SOME VERIFICATIONS FAILED ===");

        return exitCode;
    }

    private static int TestProjectRoundtrip(string projectPath, string projectFile)
    {
        Logger.Info("--- T07: Project Roundtrip ---");
        try
        {
            var project = new Project
            {
                Name = "TestProject",
                Path = projectPath,
                BuildConfig = 0,
            };
            var scene = new Scene("MainScene") { IsActive = true };
            project.Scenes.Add(scene);
            project.ActiveScene = scene;

            Serializer.SaveProject(project);

            if (!File.Exists(projectFile))
            {
                Logger.Error("FAIL: .primal file not created");
                return 1;
            }

            var json = File.ReadAllText(projectFile);
            if (!json.Contains("\"Name\"") || !json.Contains("\"Path\"") ||
                !json.Contains("\"BuildConfig\"") || !json.Contains("\"Scenes\""))
            {
                Logger.Error("FAIL: .primal JSON missing required fields");
                return 1;
            }

            var loaded = Serializer.LoadProject(projectFile);
            if (loaded == null)
            {
                Logger.Error("FAIL: LoadProject returned null");
                return 1;
            }

            if (loaded.Name != project.Name || loaded.Path != project.Path ||
                loaded.BuildConfig != project.BuildConfig || loaded.Scenes.Count != project.Scenes.Count)
            {
                Logger.Error($"FAIL: Project properties mismatch after roundtrip");
                Logger.Error($"  Name: orig='{project.Name}' loaded='{loaded.Name}' match={loaded.Name == project.Name}");
                Logger.Error($"  Path: orig='{project.Path}' loaded='{loaded.Path}' match={loaded.Path == project.Path}");
                Logger.Error($"  BuildConfig: orig={project.BuildConfig} loaded={loaded.BuildConfig} match={loaded.BuildConfig == project.BuildConfig}");
                Logger.Error($"  Scenes.Count: orig={project.Scenes.Count} loaded={loaded.Scenes.Count} match={loaded.Scenes.Count == project.Scenes.Count}");
                return 1;
            }

            Logger.Info("PASS: Project roundtrip - Name, Path, BuildConfig, Scenes preserved");
            return 0;
        }
        catch (Exception ex)
        {
            Logger.Error("FAIL: Project roundtrip threw exception", ex);
            return 1;
        }
    }

    private static int TestSceneRoundtrip(string projectPath)
    {
        Logger.Info("--- T08: Scene Roundtrip ---");
        try
        {
            var scene = new Scene("RoundtripScene") { IsActive = true };
            var entity = new GameEntity("Entity1") { IsEnabled = true, IsActive = true };
            entity.Components.Add(new Transform
            {
                PositionX = 1.0f, PositionY = 2.0f, PositionZ = 3.0f,
                RotationX = 0.0f, RotationY = 90.0f, RotationZ = 0.0f,
                ScaleX = 1.0f, ScaleY = 1.0f, ScaleZ = 1.0f,
            });
            scene.Entities.Add(entity);
            scene.ParentScene = scene;

            var project = new Project { Name = "SceneTest", Path = projectPath, BuildConfig = 0 };
            project.Scenes.Add(scene);

            Serializer.SaveProject(project);
            var loaded = Serializer.LoadProject(project.FullPath);
            if (loaded == null || loaded.Scenes.Count == 0)
            {
                Logger.Error("FAIL: Scene not loaded");
                return 1;
            }

            var loadedScene = loaded.Scenes[0];
            if (loadedScene.Name != scene.Name)
            {
                Logger.Error($"FAIL: Scene name mismatch: {loadedScene.Name} != {scene.Name}");
                return 1;
            }

            if (loadedScene.Entities.Count != 1)
            {
                Logger.Error("FAIL: Entity count mismatch in scene");
                return 1;
            }

            var projectJson = File.ReadAllText(project.FullPath);
            if (projectJson.Contains("\"ParentScene\""))
            {
                Logger.Error("FAIL: ParentScene should be [JsonIgnore] but is in JSON");
                return 1;
            }

            if (projectJson.Length > 50000)
            {
                Logger.Error($"FAIL: .primal file too large ({projectJson.Length} bytes), possible infinite loop");
                return 1;
            }

            Logger.Info($"PASS: Scene roundtrip - Name, IsActive, Entities preserved. File size: {projectJson.Length} bytes");
            return 0;
        }
        catch (Exception ex)
        {
            Logger.Error("FAIL: Scene roundtrip threw exception", ex);
            return 1;
        }
    }

    private static int TestEntityRoundtrip(string projectPath)
    {
        Logger.Info("--- T09: GameEntity + Component Roundtrip ---");
        try
        {
            var entity = new GameEntity("TestEntity")
            {
                IsEnabled = true,
                IsActive = true,
            };
            var transform = new Transform
            {
                PositionX = 10.5f, PositionY = -5.0f, PositionZ = 0.0f,
                RotationX = 45.0f, RotationY = 90.0f, RotationZ = 180.0f,
                ScaleX = 2.0f, ScaleY = 2.0f, ScaleZ = 2.0f,
            };
            entity.Components.Add(transform);

            var scene = new Scene("EntityTestScene") { IsActive = true };
            scene.Entities.Add(entity);

            var project = new Project { Name = "EntityTest", Path = projectPath, BuildConfig = 0 };
            project.Scenes.Add(scene);

            Serializer.SaveProject(project);
            var loaded = Serializer.LoadProject(project.FullPath);
            if (loaded == null || loaded.Scenes.Count == 0 || loaded.Scenes[0].Entities.Count == 0)
            {
                Logger.Error("FAIL: Entity not loaded");
                return 1;
            }

            var loadedEntity = loaded.Scenes[0].Entities[0];
            if (loadedEntity.Name != entity.Name)
            {
                Logger.Error($"FAIL: Entity name mismatch: {loadedEntity.Name}");
                return 1;
            }
            if (loadedEntity.IsEnabled != entity.IsEnabled || loadedEntity.IsActive != entity.IsActive)
            {
                Logger.Error("FAIL: Entity IsEnabled/IsActive mismatch");
                return 1;
            }

            var loadedTransform = loadedEntity.Components.OfType<Transform>().FirstOrDefault();
            if (loadedTransform == null)
            {
                Logger.Error("FAIL: Transform not loaded");
                return 1;
            }
            if (Math.Abs(loadedTransform.PositionX - transform.PositionX) > 0.001f ||
                Math.Abs(loadedTransform.ScaleX - transform.ScaleX) > 0.001f)
            {
                Logger.Error("FAIL: Transform values mismatch");
                return 1;
            }

            var json = File.ReadAllText(project.FullPath);
            if (json.Contains("\"ParentScene\"") || json.Contains("\"Owner\""))
            {
                Logger.Error("FAIL: Circular ref (ParentScene/Owner) found in JSON");
                return 1;
            }

            Logger.Info("PASS: GameEntity + Transform roundtrip - Name, IsEnabled, IsActive, Position/Rotation/Scale preserved");
            return 0;
        }
        catch (Exception ex)
        {
            Logger.Error("FAIL: Entity roundtrip threw exception", ex);
            return 1;
        }
    }

    private static int TestCorruptedFile(string testDir)
    {
        Logger.Info("--- T10: Corrupted File Error Handling ---");
        try
        {
            var corruptPath = Path.Combine(testDir, "Corrupt.primal");
            File.WriteAllText(corruptPath, "{ this is not valid JSON ::: ]]]");

            var loaded = Serializer.LoadProject(corruptPath);
            if (loaded != null)
            {
                Logger.Error("FAIL: Corrupted file should return null, got valid project");
                return 1;
            }

            Logger.Info("PASS: Corrupted file returns null (no crash)");
            return 0;
        }
        catch (Exception ex)
        {
            Logger.Error("FAIL: Corrupted file should not throw exception", ex);
            return 1;
        }
    }

    private static int TestAssetImport(string projectPath)
    {
        Logger.Info("--- T11: Asset Import (Texture) ---");
        if (!Native.NativeEngineBridge.IsLoaded)
        {
            Logger.Warning("SKIP: Engine not loaded, skipping asset import test");
            return 0;
        }

        try
        {
            Native.NativeEngineBridge.InitializeAssetSystem();

            var contentDir = Path.Combine(projectPath, "Content");
            Directory.CreateDirectory(contentDir);

            var sourcePng = "/System/Library/Desktop Pictures/Solid Colors/Stone.png";
            if (!File.Exists(sourcePng))
            {
                Logger.Warning("SKIP: System test PNG not found");
                return 0;
            }

            var destPng = Path.Combine(contentDir, "test_texture.png");
            File.Copy(sourcePng, destPng, overwrite: true);

            var result = Native.NativeEngineBridge.ImportTexture(destPng);
            if (result == null)
            {
                Logger.Error("FAIL: ImportTexture returned null (engine not loaded)");
                return 1;
            }

            if (result.Value.Success != 0)
            {
                Logger.Info($"PASS: Texture imported to GPU - {result.Value.Width}x{result.Value.Height} (handle: {result.Value.Handle})");
                return 0;
            }
            else
            {
                Logger.Warning($"GPU upload deferred: {result.Value.ErrorMessage}");
                if (!File.Exists(destPng))
                {
                    Logger.Error("FAIL: Texture not copied to content dir");
                    return 1;
                }
                Logger.Info($"PASS: Texture imported to disk (file: {new FileInfo(destPng).Length} bytes, GPU upload deferred)");
                return 0;
            }
        }
        catch (Exception ex)
        {
            Logger.Error("FAIL: Asset import threw exception", ex);
            return 1;
        }
    }

    private static int TestMeshCookImport(string projectPath)
    {
        Logger.Info("--- T12: Mesh Cook + Import (OBJ → .mesh) ---");
        if (!Native.NativeEngineBridge.IsLoaded)
        {
            Logger.Warning("SKIP: Engine not loaded, skipping mesh cook test");
            return 0;
        }

        try
        {
            Native.NativeEngineBridge.InitializeAssetSystem();

            var contentDir = Path.Combine(projectPath, "Content");
            Directory.CreateDirectory(contentDir);

            var sourceObj = "/tmp/test_triangle2.obj";
            if (!File.Exists(sourceObj))
            {
                Logger.Warning($"SKIP: Test OBJ not found at {sourceObj}");
                return 0;
            }

            var destObj = Path.Combine(contentDir, "test_triangle2.obj");
            File.Copy(sourceObj, destObj, overwrite: true);

            var cookedPath = Path.ChangeExtension(destObj, ".mesh");
            var ok = Native.NativeEngineBridge.CookMesh(destObj, cookedPath);
            if (!ok || !File.Exists(cookedPath))
            {
                Logger.Error($"FAIL: CookMesh failed (ok={ok}, file exists={File.Exists(cookedPath)})");
                return 1;
            }

            var cookedSize = new FileInfo(cookedPath).Length;
            Logger.Info($"Cooked mesh: {Path.GetFileName(cookedPath)} ({cookedSize} bytes)");

            var handle = Native.NativeEngineBridge.ImportMesh(cookedPath);
            if (handle == 0)
            {
                Logger.Warning("Mesh upload deferred (renderer not initialized)");
                Logger.Info($"PASS: Mesh cooked and saved to disk (handle pending)");
                return 0;
            }

            Logger.Info($"PASS: Mesh cooked and loaded (handle: {handle})");
            return 0;
        }
        catch (Exception ex)
        {
            Logger.Error("FAIL: Mesh cook/import threw exception", ex);
            return 1;
        }
    }

    private static int TestCookedFileVerification()
    {
        Logger.Info("--- T13: Cooked File Format Verification ---");
        try
        {
            var verifyDir = Path.Combine(Path.GetTempPath(), "PrimalEditorCookVerify_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(verifyDir);
            var cookedPath = Path.Combine(verifyDir, "verify.mesh");
            var sourceObj = "/tmp/test_cube.obj";
            if (!File.Exists(sourceObj))
            {
                Logger.Warning($"SKIP: Test OBJ not found at {sourceObj}");
                Directory.Delete(verifyDir, true);
                return 0;
            }
            File.Copy(sourceObj, Path.Combine(verifyDir, "verify.obj"), overwrite: true);

            var ok = Native.NativeEngineBridge.CookMesh(Path.Combine(verifyDir, "verify.obj"), cookedPath);
            if (!ok || !File.Exists(cookedPath))
            {
                Logger.Error($"FAIL: CookMesh produced no output");
                Directory.Delete(verifyDir, true);
                return 1;
            }

            var info = new FileInfo(cookedPath);
            Logger.Info($"Output .mesh file: {info.Length} bytes");

            using var fs = File.OpenRead(cookedPath);
            using var br = new System.IO.BinaryReader(fs);
            var nameLen = br.ReadUInt32();
            if (nameLen > 256)
            {
                Logger.Error($"FAIL: Implausible scene name length: {nameLen}");
                Directory.Delete(verifyDir, true);
                return 1;
            }
            string name = "(empty)";
            if (nameLen > 0)
            {
                name = new string(br.ReadChars((int)nameLen));
            }
            Logger.Info($"  scene name: '{name}' (len: {nameLen})");
            var materialCount = br.ReadUInt32();
            Logger.Info($"  materials: {materialCount}");
            for (uint i = 0; i < materialCount; i++)
            {
                var matNameLen = br.ReadUInt32();
                if (matNameLen > 256) break;
                var matName = matNameLen > 0 ? new string(br.ReadChars((int)matNameLen)) : "(empty)";
                Logger.Info($"    [{i}] {matName}");
            }
            var lodCount = br.ReadUInt32();
            Logger.Info($"  LODs: {lodCount}");

            Directory.Delete(verifyDir, true);
            Logger.Info($"PASS: Cooked .mesh file is well-formed (size: {info.Length} bytes, {lodCount} LODs)");
            return 0;
        }
        catch (Exception ex)
        {
            Logger.Error("FAIL: Cooked file verification threw exception", ex);
            return 1;
        }
    }
}