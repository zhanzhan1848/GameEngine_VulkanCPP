using System;
using System.IO;
using Avalonia;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Material;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Editors;

public sealed class MaterialEditorInteractionTests
{
    [Fact]
    public void ConnectionPayload_AcceptsConcreteMaterialPinTuple()
    {
        var output = new MaterialPinViewModel { NodeId = 1, PinIndex = 2, IsInput = false };
        var input = new MaterialPinViewModel { NodeId = 3, PinIndex = 4, IsInput = true };

        var accepted = MaterialEditorViewModel.TryGetConnectionPins(
            (output, input), out var actualOutput, out var actualInput);

        Assert.True(accepted);
        Assert.Same(output, actualOutput);
        Assert.Same(input, actualInput);
    }

    [Fact]
    public void PreviewPushJson_ExpandsRelativeTextureReferenceForRenderer()
    {
        // The sidecar keeps portable refs ("Content/Assets/X.texture"); the
        // native preview renderer cannot resolve them. Save used to push the
        // raw serialization, which dropped every texture binding the moment
        // the user pressed Save. This pins save-path parity with the edit
        // path (RefreshPreview/PushGraphToPreview).
        var root = Path.Combine(Path.GetTempPath(), "PrimalMatPreview_" + Guid.NewGuid().ToString("N"));
        var texturePath = Path.Combine(root, "Content", "Assets", "Lion_Albedo.texture");
        Directory.CreateDirectory(Path.GetDirectoryName(texturePath)!);
        try
        {
            File.WriteAllBytes(texturePath, Array.Empty<byte>());
            AssetRegistry.Initialize(Path.Combine(root, "Content"), root);

            const string portableGraph =
                "{\"nodes\":[{\"type\":\"ConstantTexture\",\"params\":{\"asset_path\":\"Content/Assets/Lion_Albedo.texture\"}}]," +
                "\"connections\":[]}";

            var prepared = MaterialEditorViewModel.PreviewPushJson(portableGraph);

            Assert.Contains(texturePath, prepared);
            Assert.StartsWith("{\"nodes\":[{\"type\":\"ConstantTexture\",\"params\":{\"asset_path\":\"/", prepared);
            // The raw input must stay untouched — it is what lands on disk.
            Assert.NotEqual(portableGraph, prepared);
        }
        finally
        {
            AssetRegistry.Reset();
            try { Directory.Delete(root, recursive: true); } catch { /* best effort */ }
        }
    }

    [Fact]
    public void ConnectionPayload_RejectsNonConnectorTuple()
    {
        Assert.False(MaterialEditorViewModel.TryGetConnectionPins(
            ("output", "input"), out _, out _));
    }

    [Fact]
    public void RightButtonStrokeCrossingConnection_IsDetected()
    {
        Assert.True(MaterialEditorView.ConnectionIntersectsStroke(
            new Point(0, 0),
            new Point(200, 0),
            new Point(100, -20),
            new Point(100, 20),
            tolerance: 9));
    }

    [Fact]
    public void RightClickNearConnection_IsDetected()
    {
        Assert.True(MaterialEditorView.ConnectionIntersectsStroke(
            new Point(0, 0),
            new Point(200, 0),
            new Point(100, 5),
            new Point(100, 5),
            tolerance: 9));
    }

    [Fact]
    public void StrokeAwayFromConnection_IsIgnored()
    {
        Assert.False(MaterialEditorView.ConnectionIntersectsStroke(
            new Point(0, 0),
            new Point(200, 0),
            new Point(100, 40),
            new Point(100, 80),
            tolerance: 9));
    }
}
