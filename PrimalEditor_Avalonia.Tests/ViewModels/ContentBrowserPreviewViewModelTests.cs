using System;
using System.Collections.Generic;
using System.IO;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Native;
using PrimalEditor_Avalonia.ViewModels;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.ViewModels;

public sealed class ContentBrowserPreviewViewModelTests
{
    [Theory]
    [InlineData("/path/to/foo.model.json",          "/path/to/foo.mesh")]
    [InlineData("/path/to/corridor-corner.model.json", "/path/to/corridor-corner.mesh")]
    [InlineData("C:\\Users\\foo\\bar.model.json",   "C:\\Users\\foo\\bar.mesh")]
    [InlineData("/path/to/standalone",                "/path/to/standalone.mesh")]
    [InlineData("/path/to/foo.fbx",                   "/path/to/foo.mesh")]
    public void SiblingMeshPath_strips_dot_model_json_suffix(string sidecar, string expected)
    {
        Assert.Equal(expected, ContentBrowserPreviewViewModel.SiblingMeshPath(sidecar));
    }

    [Fact]
    public void LoadMesh_resolves_model_json_via_sidecar_to_cooked_mesh()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalPreview_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var sidecarPath = Path.Combine(dir, "foo.model.json");
            var meshPath = Path.Combine(dir, "foo.mesh");
            File.WriteAllText(sidecarPath,
                $"{{ \"version\": 1, \"source\": \"foo.fbx\", \"mesh\": \"{meshPath}\", \"submeshes\": [] }}");
            File.WriteAllBytes(meshPath, Array.Empty<byte>());

            var vm = new ContentBrowserPreviewViewModel();
            vm.Asset = new AssetInfo { Type = AssetType.Model, FullPath = sidecarPath };

            // Path resolution succeeded (the sidecar pointed at a real
            // mesh file) — the placeholder mesh is empty so Geometry.Load
            // can't parse it, but the resolver itself is verified by
            // *not* getting "Cooked .mesh file missing on disk".
            Assert.Equal("Could not parse mesh file", vm.StatusMessage);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void LoadMesh_falls_back_to_sibling_when_no_sidecar()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalPreview_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var fbxPath = Path.Combine(dir, "foo.fbx");
            var meshPath = Path.Combine(dir, "foo.mesh");
            File.WriteAllBytes(fbxPath, Array.Empty<byte>());
            File.WriteAllBytes(meshPath, Array.Empty<byte>());

            var vm = new ContentBrowserPreviewViewModel();
            vm.Asset = new AssetInfo { Type = AssetType.Mesh, FullPath = fbxPath };

            Assert.Equal("Could not parse mesh file", vm.StatusMessage);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void LoadMesh_reports_missing_when_neither_sidecar_nor_sibling_exists()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalPreview_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var fbxPath = Path.Combine(dir, "ghost.fbx");
            File.WriteAllBytes(fbxPath, Array.Empty<byte>());

            var vm = new ContentBrowserPreviewViewModel();
            vm.Asset = new AssetInfo { Type = AssetType.Mesh, FullPath = fbxPath };

            Assert.Equal("Cooked .mesh file missing on disk", vm.StatusMessage);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void LoadMesh_MalformedCookedMesh_FailsPreviewWithoutThrowing()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalPreview_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var sidecarPath = Path.Combine(dir, "broken.model.json");
            var meshPath = Path.Combine(dir, "broken.mesh");
            File.WriteAllText(sidecarPath,
                $"{{ \"version\": 1, \"source\": \"broken.fbx\", \"mesh\": \"{meshPath}\", \"submeshes\": [] }}");

            // A corrupt/old cooked file used to flow into the wireframe
            // render callback, where invalid indices could escape as an
            // unhandled exception and terminate the Avalonia process.
            using (var fs = File.Create(meshPath))
            using (var bw = new BinaryWriter(fs))
            {
                bw.Write(0u); // empty scene name
                bw.Write(0u); // material count
                bw.Write(1u); // LOD count
                bw.Write(0u); // empty LOD name
                bw.Write(uint.MaxValue); // impossible mesh count
            }

            var vm = new ContentBrowserPreviewViewModel();
            var exception = Record.Exception(() => vm.Asset = new AssetInfo
            {
                Type = AssetType.Model,
                FullPath = sidecarPath,
            });

            Assert.Null(exception);
            Assert.Equal("Could not parse mesh file", vm.StatusMessage);
            Assert.Null(vm.PreviewLOD);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void Asset_setter_refreshes_when_switching_between_distinct_assets()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalPreview_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var meshA = Path.Combine(dir, "a.mesh");
            var meshB = Path.Combine(dir, "b.mesh");
            File.WriteAllBytes(meshA, Array.Empty<byte>());
            File.WriteAllBytes(meshB, Array.Empty<byte>());
            var fbxA = Path.Combine(dir, "a.fbx");
            var fbxB = Path.Combine(dir, "b.fbx");
            File.WriteAllBytes(fbxA, Array.Empty<byte>());
            File.WriteAllBytes(fbxB, Array.Empty<byte>());

            var vm = new ContentBrowserPreviewViewModel();
            vm.Asset = new AssetInfo { Type = AssetType.Mesh, FullPath = fbxA };
            Assert.Equal("Could not parse mesh file", vm.StatusMessage);

            // A different AssetInfo instance with a different path must
            // re-run LoadMesh (otherwise switching between two .model.json
            // files in the browser shows the older wireframe forever).
            vm.Asset = new AssetInfo { Type = AssetType.Mesh, FullPath = fbxB };
            Assert.Equal("Could not parse mesh file", vm.StatusMessage);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void Asset_setter_Refreshes_via_PropertyChanged_observable()
    {
        // Mimics the binding chain the Avalonia view uses: ContentControl
        // isVisible binding to Type, MeshPreviewCanvas binding to ViewModel.
        // If Asset setter flushes PropertyChanged, downstream consumers can
        // observe the new state. This guards against a regression where
        // Refresh() runs but consumers never see the new values.
        var dir = Path.Combine(Path.GetTempPath(), "PrimalPreview_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var fbxA = Path.Combine(dir, "a.fbx");
            var fbxB = Path.Combine(dir, "b.fbx");
            File.WriteAllBytes(fbxA, Array.Empty<byte>());
            File.WriteAllBytes(fbxB, Array.Empty<byte>());

            var vm = new ContentBrowserPreviewViewModel();
            var typeChanges = new List<AssetType>();
            vm.PropertyChanged += (_, e) =>
            {
                if (e.PropertyName == nameof(ContentBrowserPreviewViewModel.Type))
                    typeChanges.Add(vm.Type);
            };

            vm.Asset = new AssetInfo { Type = AssetType.Mesh, FullPath = fbxA };
            vm.Asset = new AssetInfo { Type = AssetType.Texture, FullPath = Path.Combine(dir, "x.png") };

            // Each Asset setter call must produce a Type PropertyChanged so
            // the XAML DataTrigger (IsTexture/IsMeshOrModel/...) re-evaluates.
            Assert.True(typeChanges.Count >= 2,
                $"Expected 2 Type changes, got {typeChanges.Count}: [{string.Join(",", typeChanges)}]");
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void SetAssetAndRefresh_reloads_even_when_reference_is_unchanged()
    {
        // The Asset setter's early-return guard is correct for normal
        // use, but the submesh click handler routes through SelectSubmesh
        // with the same parent ref twice in a row and needs a real
        // re-render each time. SetAssetAndRefresh bypasses the guard.
        var dir = Path.Combine(Path.GetTempPath(), "PrimalPreview_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var path = Path.Combine(dir, "a.fbx");
            File.WriteAllBytes(path, Array.Empty<byte>());
            var asset = new AssetInfo { Type = AssetType.Mesh, FullPath = path };

            var vm = new ContentBrowserPreviewViewModel();
            vm.Asset = asset;

            int refreshCount = 0;
            vm.PropertyChanged += (_, _) => { if (vm.Type != AssetType.Unknown) refreshCount++; };
            vm.SetAssetAndRefresh(asset);

            Assert.True(refreshCount > 0, "SetAssetAndRefresh must notify PropertyChanged even on identical ref");
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    public void AssetInfo_IsExpanded_toggles_and_notifies()
    {
        // Regression: clicking a .model.json should toggle its expansion
        // state so the submesh tiles appear/disappear inline. The toggle
        // must fire PropertyChanged so the XAML binding re-evaluates.
        var asset = new AssetInfo
        {
            Type = AssetType.Model,
            FullPath = "/tmp/foo.model.json",
            Children = { new AssetInfo { Type = AssetType.Mesh, FullPath = "/tmp/foo.submesh/bar" } },
        };

        Assert.False(asset.IsExpanded);

        var changes = 0;
        asset.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(AssetInfo.IsExpanded)) changes++; };

        asset.IsExpanded = true;
        Assert.True(asset.IsExpanded);
        Assert.Equal(1, changes);

        // Same value — should NOT fire
        asset.IsExpanded = true;
        Assert.Equal(1, changes);

        // Toggling back fires
        asset.IsExpanded = false;
        Assert.False(asset.IsExpanded);
        Assert.Equal(2, changes);
    }

    [Fact]
    public void AssetInfo_IsExpanded_multiple_instances_are_independent()
    {
        var a = new AssetInfo { Type = AssetType.Model, FullPath = "/tmp/a.model.json" };
        var b = new AssetInfo { Type = AssetType.Model, FullPath = "/tmp/b.model.json" };

        a.IsExpanded = true;
        b.IsExpanded = true;

        foreach (var info in new[] { a, b })
            if (info != b) info.IsExpanded = false;

        Assert.False(a.IsExpanded);
        Assert.True(b.IsExpanded);
    }

    [Fact]
    public void ContentBrowserViewModel_ExpandPathTo_marks_ancestors_expanded()
    {
        // Reproduce the Folders tree shape without a real Project:
        //   root
        //   └─ child
        //      └─ grandchild (target)
        var root = new ContentFolder { Name = "root", FullPath = "/r" };
        var child = new ContentFolder { Name = "child", FullPath = "/r/c" };
        var grandchild = new ContentFolder { Name = "grandchild", FullPath = "/r/c/g" };
        child.Subfolders.Add(grandchild);
        root.Subfolders.Add(child);

        var vm = new ContentBrowserViewModel();
        // Inject the synthetic tree (bypass Project loading).
        vm.Folders.Add(root);

        vm.ExpandPathTo(grandchild);

        Assert.True(root.IsExpanded);
        Assert.True(child.IsExpanded);
        Assert.False(grandchild.IsExpanded, "the target itself should not be flagged as expanded");
    }

    [Fact]
    public void ContentBrowserViewModel_ExpandPathTo_is_noop_when_target_missing()
    {
        // When the target folder isn't part of the Folders tree (the
        // middle panel stub path creates a transient ContentFolder), the
        // sync must be a no-op so existing expanded state isn't disturbed.
        var root = new ContentFolder { Name = "root", FullPath = "/r" };
        root.IsExpanded = true;

        var vm = new ContentBrowserViewModel();
        vm.Folders.Add(root);

        var orphan = new ContentFolder { Name = "orphan", FullPath = "/not/in/tree" };
        vm.ExpandPathTo(orphan);

        Assert.True(root.IsExpanded, "previously-expanded state must be preserved");
    }
}
