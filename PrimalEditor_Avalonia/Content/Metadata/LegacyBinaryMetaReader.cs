using System;
using System.IO;
using System.Text.Json.Nodes;
using PrimalEditor_Avalonia.Common;
using PrimalEditor_Avalonia.Content;
using PrimalEditor_Avalonia.Editors.Geometry;
using PrimalEditor_Avalonia.Editors.Texture;
using PrimalEditor_Avalonia.Editors.PrimitiveMesh;

namespace PrimalEditor_Avalonia.Content.Metadata;

public sealed class LegacyBinaryMetaReader
{
    public readonly struct ReadResult
    {
        public bool Success { get; init; }
        public AssetMetaDocument? Document { get; init; }
    }

    public ReadResult TryRead(string metaPath, string importer)
    {
        if (string.IsNullOrEmpty(importer))
        {
            throw new ArgumentNullException(nameof(importer));
        }

        if (!File.Exists(metaPath))
        {
            return new ReadResult { Success = false };
        }

        byte[] originalBytes;
        try
        {
            originalBytes = File.ReadAllBytes(metaPath);
        }
        catch
        {
            return new ReadResult { Success = false };
        }

        try
        {
            using var ms = new MemoryStream(originalBytes);
            using var br = new BinaryReader(ms);

            var magic = br.ReadUInt32();
            if (magic != AssetImportSettingsStore.Magic)
            {
                return new ReadResult { Success = false };
            }

            var version = br.ReadUInt32();
            if (version != AssetImportSettingsStore.CurrentVersion)
            {
                return new ReadResult { Success = false };
            }

            IAssetImportSettings? settings = importer switch
            {
                "TextureImporter" => new TextureImportSettings(),
                "GeometryImporter" => new GeometryImportSettings(),
                "PrimitiveMeshImporter" => new PrimitiveInitInfo(),
                _ => null
            };

            if (settings == null)
            {
                return new ReadResult { Success = false };
            }

            settings.FromBinary(br);

            var settingsJson = ConvertSettingsToJson(settings);
            var document = new AssetMetaDocument
            {
                MetaVersion = 1,
                Guid = new AssetGuid(Guid.NewGuid()),
                Importer = importer,
                Settings = settingsJson
            };

            return new ReadResult { Success = true, Document = document };
        }
        catch
        {
            try
            {
                File.WriteAllBytes(metaPath, originalBytes);
            }
            catch
            {
            }

            return new ReadResult { Success = false };
        }
    }

    private static JsonObject? ConvertSettingsToJson(IAssetImportSettings settings)
    {
        if (settings is TextureImportSettings textureSettings)
        {
            var sourcesArray = new JsonArray();
            foreach (var source in textureSettings.Sources)
            {
                sourcesArray.Add(source);
            }

            return new JsonObject
            {
                ["Sources"] = sourcesArray,
                ["Dimension"] = (int)textureSettings.Dimension,
                ["MipLevels"] = textureSettings.MipLevels,
                ["AlphaThreshold"] = textureSettings.AlphaThreshold,
                ["PreferBC7"] = textureSettings.PreferBC7,
                ["OutputFormat"] = (int)textureSettings.OutputFormat,
                ["Compress"] = textureSettings.Compress,
                ["IsSRGB"] = textureSettings.IsSRGB,
                ["CompressionMode"] = (int)textureSettings.CompressionMode,
                ["ArraySize"] = textureSettings.ArraySize,
                ["Depth"] = textureSettings.Depth,
                ["IsNormalMap"] = textureSettings.IsNormalMap
            };
        }

        if (settings is GeometryImportSettings geometrySettings)
        {
            return new JsonObject
            {
                ["LodMode"] = (int)geometrySettings.LodMode,
                ["Lod0Threshold"] = geometrySettings.Lod0Threshold,
                ["Lod1Threshold"] = geometrySettings.Lod1Threshold,
                ["Lod2Threshold"] = geometrySettings.Lod2Threshold,
                ["GenerateTangentSpace"] = geometrySettings.GenerateTangentSpace,
                ["RecalculateNormals"] = geometrySettings.RecalculateNormals,
                ["Compression"] = (int)geometrySettings.Compression,
                ["ForceSingleLod"] = geometrySettings.ForceSingleLod
            };
        }

        if (settings is PrimitiveInitInfo primitiveSettings)
        {
            return new JsonObject
            {
                ["Type"] = (int)primitiveSettings.Type,
                ["SegmentsX"] = primitiveSettings.SegmentsX,
                ["SegmentsY"] = primitiveSettings.SegmentsY,
                ["SegmentsZ"] = primitiveSettings.SegmentsZ,
                ["SizeX"] = primitiveSettings.SizeX,
                ["SizeY"] = primitiveSettings.SizeY,
                ["SizeZ"] = primitiveSettings.SizeZ,
                ["LOD"] = primitiveSettings.LOD,
                ["SmoothingAngle"] = primitiveSettings.SmoothingAngle,
                ["OutputName"] = primitiveSettings.OutputName ?? "Primitive_Mesh"
            };
        }

        return null;
    }
}