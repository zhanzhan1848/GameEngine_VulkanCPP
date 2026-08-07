using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Content.Materials;
using PrimalEditor_Avalonia.Native;
using MeshGeometry = PrimalEditor_Avalonia.Editors.Geometry.Geometry;
using MeshLOD = PrimalEditor_Avalonia.Editors.Geometry.MeshLOD;

namespace PrimalEditor_Avalonia.ViewModels;

/// <summary>
/// Side-pane preview view model for the Content Browser. Branches on
/// <see cref="AssetInfo.Type"/> and exposes simple render-friendly
/// properties so the view can drive a content-preserving XAML
/// template (image / wireframe / swatch / text panel).
///
/// Heavy GPU surfaces are deliberately avoided: textures decode to
/// a CPU bitmap, meshes use the pure-CPU <see>WireframeRenderer</see>,
/// materials render an Avalonia brush swatch plus metadata. This keeps
/// the side pane cheap to embed alongside the main 3D viewport.
/// </summary>
public class ContentBrowserPreviewViewModel : ViewModelBase, IDisposable
{
    private AssetInfo? _asset;
    private Bitmap? _textureBitmap;
    private MeshGeometry? _geometry;
    private MaterialDocument? _material;
    private string _scriptPreview = string.Empty;
    private string _shaderPreview = string.Empty;
    private string _audioSummary = string.Empty;
    private IBrush _materialSwatch = Brushes.Gray;
    private string _materialSummary = string.Empty;
    private string _meshSummary = string.Empty;
    private string _statusMessage = string.Empty;
    private float _meshYaw;
    private DispatcherTimer? _spinTimer;

    public AssetInfo? Asset
    {
        get => _asset;
        set
        {
            if (_asset == value) return;
            _asset = value;
            OnPropertyChanged(nameof(Asset));
            OnPropertyChanged(nameof(HasAsset));
            OnPropertyChanged(nameof(Type));
            Refresh();
        }
    }

    public bool HasAsset => _asset != null;

    public AssetType Type => _asset?.Type ?? AssetType.Unknown;

    public Bitmap? TextureBitmap
    {
        get => _textureBitmap;
        private set { _textureBitmap = value; OnPropertyChanged(); OnPropertyChanged(nameof(HasTextureBitmap)); }
    }

    public bool HasTextureBitmap => _textureBitmap != null;

    public MeshLOD? PreviewLOD
    {
        get
        {
            if (_geometry == null) return null;
            var idx = Math.Clamp(_meshLodIndex, 0, Math.Max(0, _geometry.LODs.Count - 1));
            return _geometry.LODs.Count > 0 ? _geometry.LODs[idx] : null;
        }
    }

    private int _meshLodIndex;

    public int MeshLodIndex
    {
        get => _meshLodIndex;
        set
        {
            var clamped = Math.Max(0, value);
            if (_meshLodIndex != clamped) { _meshLodIndex = clamped; OnPropertyChanged(); OnPropertyChanged(nameof(PreviewLOD)); }
        }
    }

    public IReadOnlyList<int> MeshLodIndices
    {
        get
        {
            if (_geometry == null) return Array.Empty<int>();
            var list = new List<int>();
            for (var i = 0; i < _geometry.LODs.Count; i++) list.Add(i);
            return list;
        }
    }

    public string MeshSummary
    {
        get => _meshSummary;
        private set { _meshSummary = value; OnPropertyChanged(); }
    }

    public float MeshYaw
    {
        get => _meshYaw;
        private set { _meshYaw = value; OnPropertyChanged(); }
    }

    public MaterialDocument? Material
    {
        get => _material;
        private set { _material = value; OnPropertyChanged(); }
    }

    public IBrush MaterialSwatch
    {
        get => _materialSwatch;
        private set { _materialSwatch = value; OnPropertyChanged(); }
    }

    public string MaterialSummary
    {
        get => _materialSummary;
        private set { _materialSummary = value; OnPropertyChanged(); }
    }

    public string ScriptPreview
    {
        get => _scriptPreview;
        private set { _scriptPreview = value; OnPropertyChanged(); }
    }

    public string ShaderPreview
    {
        get => _shaderPreview;
        private set { _shaderPreview = value; OnPropertyChanged(); }
    }

    public string AudioSummary
    {
        get => _audioSummary;
        private set { _audioSummary = value; OnPropertyChanged(); }
    }

    public string StatusMessage
    {
        get => _statusMessage;
        private set { _statusMessage = value; OnPropertyChanged(); OnPropertyChanged(nameof(HasStatus)); }
    }

    public bool HasStatus => !string.IsNullOrEmpty(_statusMessage);

    public void Refresh()
    {
        StopSpin();

        TextureBitmap = null;
        _geometry = null;
        OnPropertyChanged(nameof(PreviewLOD));
        OnPropertyChanged(nameof(MeshLodIndices));
        Material = null;
        MaterialSwatch = Brushes.Gray;
        MaterialSummary = string.Empty;
        MeshSummary = string.Empty;
        ScriptPreview = string.Empty;
        ShaderPreview = string.Empty;
        AudioSummary = string.Empty;
        StatusMessage = string.Empty;
        MeshLodIndex = 0;
        OnPropertyChanged(nameof(MeshLodIndices));

        var info = _asset;
        if (info == null) return;

        try
        {
            switch (info.Type)
            {
                case AssetType.Texture:
                    LoadTexture(info);
                    break;
                case AssetType.Mesh:
                    LoadMesh(info);
                    break;
                case AssetType.Material:
                    LoadMaterial(info);
                    break;
                case AssetType.Script:
                    LoadScript(info);
                    break;
                case AssetType.Shader:
                    LoadShader(info);
                    break;
                case AssetType.Audio:
                    LoadAudio(info);
                    break;
                default:
                    StatusMessage = $"No preview for {info.Type}";
                    break;
            }
        }
        catch (Exception ex)
        {
            StatusMessage = $"Preview failed: {ex.Message}";
            Logger.Warning($"ContentBrowserPreview failed for {info.FileName}: {ex.Message}");
        }
    }

    private void LoadTexture(AssetInfo info)
    {
        if (!File.Exists(info.FullPath)) { StatusMessage = "Texture file missing on disk"; return; }

        var ext = Path.GetExtension(info.FullPath).ToLowerInvariant();

        if (ext == ".texture")
        {
            if (PrimalEditor_Avalonia.Import.TextureTexrReader.TryDecodeBitmap(info.FullPath, out var texBmp, out var texErr) && texBmp != null)
            {
                TextureBitmap = texBmp;
                StatusMessage = $"{texBmp.PixelSize.Width}×{texBmp.PixelSize.Height} (engine texture)";
            }
            else
            {
                StatusMessage = $"Preview unavailable: {texErr ?? "TEXR decode failed"}";
            }
            return;
        }

        // Only decode formats Avalonia can open natively without an
        // importer sidecar. TGA/DDS/HDR show a "convert before preview"
        // hint instead of failing the whole pane.
        var decodable = ext is ".png" or ".jpg" or ".jpeg" or ".bmp" or ".gif";
        if (!decodable)
        {
            StatusMessage = $"Preview unavailable for {ext} (re-import to enable)";
            return;
        }

        try
        {
            TextureBitmap = new Bitmap(info.FullPath);
            StatusMessage = $"{TextureBitmap.PixelSize.Width}×{TextureBitmap.PixelSize.Height}";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Decode failed: {ex.Message}";
            TextureBitmap = null;
        }
    }

    private void LoadMesh(AssetInfo info)
    {
        if (!File.Exists(info.FullPath)) { StatusMessage = "Mesh file missing on disk"; return; }

        var geom = new MeshGeometry();
        if (!geom.Load(info.FullPath))
        {
            StatusMessage = "Could not parse mesh file";
            return;
        }
        _geometry = geom;
        OnPropertyChanged(nameof(PreviewLOD));
        OnPropertyChanged(nameof(MeshLodIndices));
        var lodCount = geom.LODs.Count;
        var vertexSum = geom.LODs.Sum(l => l.Meshes.Sum(m => m.VertexCount));
        var triSum = geom.LODs.Sum(l => l.Meshes.Sum(m => m.IndexCount / 3));
        MeshSummary = $"LODs: {lodCount}   Vertices: {vertexSum:N0}   Triangles: {triSum:N0}";
        StatusMessage = $"{info.FileName}";
        StartSpin();
    }

    private void LoadMaterial(AssetInfo info)
    {
        if (!File.Exists(info.FullPath)) { StatusMessage = "Material file missing on disk"; return; }

        var doc = MaterialSerializer.Read(info.FullPath);
        if (doc == null) { StatusMessage = "Could not parse material"; return; }
        Material = doc;
        MaterialSwatch = ResolveMaterialSwatch(doc);
        MaterialSummary = BuildMaterialSummary(doc);
        StatusMessage = string.Empty;
    }

    private static IBrush ResolveMaterialSwatch(MaterialDocument doc)
    {
        // Prefer a known base-color slot, otherwise the first color
        // parameter found, otherwise a neutral grey. Swatch is purely
        // informational — a real GPU render requires the engine.
        string[] colorSlots = { "baseColor", "albedo", "diffuse", "color" };
        foreach (var slot in colorSlots)
        {
            if (doc.Parameters.TryGetValue(slot, out var value) && value is MaterialColorParameter c)
                return new SolidColorBrush(Color.FromRgb(ClampByte(c.R), ClampByte(c.G), ClampByte(c.B)));
        }
        foreach (var value in doc.Parameters.Values)
        {
            if (value is MaterialColorParameter c2)
                return new SolidColorBrush(Color.FromRgb(ClampByte(c2.R), ClampByte(c2.G), ClampByte(c2.B)));
        }
        return doc.RenderState.Surface == "Opaque" ? Brushes.SlateGray : Brushes.DimGray;
    }

    private static byte ClampByte(float v) => (byte)Math.Clamp((int)MathF.Round(v * 255f), 0, 255);

    private static string BuildMaterialSummary(MaterialDocument doc)
    {
        var lines = new List<string>
        {
            $"Shader: {doc.ShaderGuid.Value:D}",
            $"Surface: {doc.RenderState.Surface}",
            $"Cull: {doc.RenderState.CullMode}",
            $"Depth: test={doc.RenderState.DepthTest} write={doc.RenderState.DepthWrite}",
        };
        if (doc.Parameters.Count > 0)
        {
            lines.Add("Parameters:");
            foreach (var (k, v) in doc.Parameters)
            {
                lines.Add($"  {k}: {DescribeParam(v)}");
            }
        }
        if (doc.Textures.Count > 0)
        {
            lines.Add("Textures:");
            foreach (var (k, v) in doc.Textures)
                lines.Add($"  {k}: {v.Value:D}");
        }
        return string.Join("\n", lines);
    }

    private static string DescribeParam(MaterialParameterValue v) => v switch
    {
        MaterialScalarParameter s => s.Scalar.ToString("0.###"),
        MaterialColorParameter c => $"rgb({c.R:0.##},{c.G:0.##},{c.B:0.##})",
        _ => "(unknown)",
    };

    private void LoadScript(AssetInfo info)
    {
        if (!File.Exists(info.FullPath)) { StatusMessage = "Script file missing on disk"; return; }
        try
        {
            // Keep the preview small — first 4 KB is plenty to identify
            // the file visually. Loading the full text would jank the UI
            // for big scripts.
            using var fs = File.OpenRead(info.FullPath);
            using var sr = new StreamReader(fs);
            var buf = new char[Math.Min(4096, (int)Math.Min(fs.Length, 4096))];
            var read = sr.Read(buf, 0, buf.Length);
            ScriptPreview = read > 0 ? new string(buf, 0, read) : "(empty)";
            StatusMessage = $"{info.FileName}";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Read failed: {ex.Message}";
        }
    }

    private void LoadShader(AssetInfo info)
    {
        if (!File.Exists(info.FullPath)) { StatusMessage = "Shader file missing on disk"; return; }
        try
        {
            // For shaders, surface the first 32 lines — enough to
            // recognize which shader stage and entry point the file is.
            var lines = File.ReadAllLines(info.FullPath);
            var take = Math.Min(32, lines.Length);
            ShaderPreview = string.Join('\n', lines, 0, take) + (lines.Length > take ? "\n…" : string.Empty);
            StatusMessage = $"{info.FileName}";
        }
        catch (Exception ex)
        {
            StatusMessage = $"Read failed: {ex.Message}";
        }
    }

    private void LoadAudio(AssetInfo info)
    {
        // Audio doesn't decode cheaply without an importer; surface
        // metadata plus file size so the user at least sees the asset
        // they selected. Real waveform would require a decoder.
        if (!File.Exists(info.FullPath)) { StatusMessage = "Audio file missing on disk"; return; }
        var size = new FileInfo(info.FullPath).Length;
        AudioSummary = $"Format: {Path.GetExtension(info.FullPath).TrimStart('.').ToUpperInvariant()}\nSize: {size / 1024.0:F1} KB\n\n(Waveform preview requires engine importer)";
        StatusMessage = $"{info.FileName}";
    }

    private void StartSpin()
    {
        _spinTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(40) };
        _spinTimer.Tick += (_, _) =>
        {
            MeshYaw = (MeshYaw + 0.02f) % (MathF.PI * 2f);
        };
        _spinTimer.Start();
    }

    private void StopSpin()
    {
        if (_spinTimer != null)
        {
            _spinTimer.Stop();
            _spinTimer = null;
        }
        MeshYaw = 0f;
    }

    public void Dispose()
    {
        StopSpin();
    }
}