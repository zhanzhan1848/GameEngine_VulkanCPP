using System.Collections.Generic;
using System.IO;
using PrimalEditor_Avalonia.Native.Importers;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Import;

/// <summary>
/// asset-pipeline S1：重导入必须保留用户已编辑的默认材质（绝不覆盖）。
/// 派生图恢复（材质在、图缺失）需要引擎会话，测试环境不加载引擎——
/// 本组锁定的是"保留"契约：重导入调用后用户材质字节不变，且不会在
/// 无引擎时凭空生成派生件。
/// </summary>
public sealed class MeshReimportPreserveTests
{
    [Fact]
    public void CreateDefaultMaterial_KeepsUserEditedMaterialFile()
    {
        var dir = Path.Combine(Path.GetTempPath(), "PrimalReimport_" + Path.GetRandomFileName());
        Directory.CreateDirectory(dir);
        try
        {
            const string userEdited = "{\"SchemaVersion\":2,\"Name\":\"user-tweaked\",\"Textures\":{}}";
            var materialPath = MeshImporterPlugin.DefaultMaterialPath(dir, "robot");
            File.WriteAllText(materialPath, userEdited);

            // 模拟重导入：同一 sourceStem 再次走默认材质创建（引擎未加载）。
            MeshImporterPlugin.CreateDefaultMaterial(
                dir, "robot",
                sidecarPath: Path.Combine(dir, "robot.model.json"),
                cookedTextures: new Dictionary<string, string>());

            Assert.Equal(userEdited, File.ReadAllText(materialPath));
            // 无引擎时不做图恢复——不产生新派生件。
            Assert.False(File.Exists(Path.Combine(dir, "robot.material.material.graph.json")));
        }
        finally
        {
            Directory.Delete(dir, recursive: true);
        }
    }
}
