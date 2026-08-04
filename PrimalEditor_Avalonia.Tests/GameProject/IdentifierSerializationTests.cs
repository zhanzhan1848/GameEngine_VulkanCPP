using System;
using System.Text.Json;
using Xunit;
using PrimalEditor_Avalonia.GameProject.Identifiers;
using PrimalEditor_Avalonia.Content;

namespace PrimalEditor_Avalonia.Tests.GameProject
{
    public class IdentifierSerializationTests
    {
        [Fact]
        public void ProjectGuid_RejectsEmptyInConstructor()
        {
            Assert.Throws<ArgumentException>(() => new ProjectGuid(Guid.Empty));
        }

        [Fact]
        public void ProjectGuid_RejectsEmptyInParse()
        {
            Assert.Throws<ArgumentException>(() => ProjectGuid.Parse(Guid.Empty.ToString("D")));
        }

        [Fact]
        public void ProjectGuid_SerializesToLowercaseDFormat()
        {
            var guid = new ProjectGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var options = new JsonSerializerOptions();
            options.Converters.Add(new ProjectGuid.JsonConverter());

            var json = JsonSerializer.Serialize(guid, options);
            Assert.Contains("d5a5b8c2-8d2f-4a3e-9b1c-7f6d5a4b3c2e", json.ToLower());
        }

        [Fact]
        public void ProjectGuid_RoundTripSerialization()
        {
            var guid = new ProjectGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var options = new JsonSerializerOptions();
            options.Converters.Add(new ProjectGuid.JsonConverter());

            var json = JsonSerializer.Serialize(guid, options);
            var deserialized = JsonSerializer.Deserialize<ProjectGuid>(json, options);
            Assert.Equal(guid, deserialized);
        }

        [Fact]
        public void SceneGuid_RejectsEmptyInConstructor()
        {
            Assert.Throws<ArgumentException>(() => new SceneGuid(Guid.Empty));
        }

        [Fact]
        public void SceneGuid_RejectsEmptyInParse()
        {
            Assert.Throws<ArgumentException>(() => SceneGuid.Parse(Guid.Empty.ToString("D")));
        }

        [Fact]
        public void SceneGuid_SerializesToLowercaseDFormat()
        {
            var guid = new SceneGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var options = new JsonSerializerOptions();
            options.Converters.Add(new SceneGuid.JsonConverter());

            var json = JsonSerializer.Serialize(guid, options);
            Assert.Contains("d5a5b8c2-8d2f-4a3e-9b1c-7f6d5a4b3c2e", json.ToLower());
        }

        [Fact]
        public void SceneGuid_RoundTripSerialization()
        {
            var guid = new SceneGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var options = new JsonSerializerOptions();
            options.Converters.Add(new SceneGuid.JsonConverter());

            var json = JsonSerializer.Serialize(guid, options);
            var deserialized = JsonSerializer.Deserialize<SceneGuid>(json, options);
            Assert.Equal(guid, deserialized);
        }

        [Fact]
        public void EntityGuid_RejectsEmptyInConstructor()
        {
            Assert.Throws<ArgumentException>(() => new EntityGuid(Guid.Empty));
        }

        [Fact]
        public void EntityGuid_RejectsEmptyInParse()
        {
            Assert.Throws<ArgumentException>(() => EntityGuid.Parse(Guid.Empty.ToString("D")));
        }

        [Fact]
        public void EntityGuid_SerializesToLowercaseDFormat()
        {
            var guid = new EntityGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var options = new JsonSerializerOptions();
            options.Converters.Add(new EntityGuid.JsonConverter());

            var json = JsonSerializer.Serialize(guid, options);
            Assert.Contains("d5a5b8c2-8d2f-4a3e-9b1c-7f6d5a4b3c2e", json.ToLower());
        }

        [Fact]
        public void EntityGuid_RoundTripSerialization()
        {
            var guid = new EntityGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var options = new JsonSerializerOptions();
            options.Converters.Add(new EntityGuid.JsonConverter());

            var json = JsonSerializer.Serialize(guid, options);
            var deserialized = JsonSerializer.Deserialize<EntityGuid>(json, options);
            Assert.Equal(guid, deserialized);
        }

        [Fact]
        public void AssetGuid_AcceptsEmptyAsUnsetSentinel()
        {
            // AssetGuid is now a value type that uses Guid.Empty as the
            // canonical "not pointing at anything" sentinel, mirroring
            // MeshGuid / MaterialGuid fields where the empty value
            // means "no asset bound". Components (e.g.
            // MaterialOverrideSlot) need a permissive default; the
            // editor is the layer responsible for validating that
            // references into the AssetRegistry are non-empty.
            var empty = new AssetGuid(Guid.Empty);
            Assert.Equal(Guid.Empty, empty.Value);
            var parsed = AssetGuid.Parse(Guid.Empty.ToString("D"));
            Assert.Equal(Guid.Empty, parsed.Value);
        }

        [Fact]
        public void AssetGuid_SerializesToLowercaseDFormat()
        {
            var guid = new AssetGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var options = new JsonSerializerOptions();
            options.Converters.Add(new AssetGuid.JsonConverter());

            var json = JsonSerializer.Serialize(guid, options);
            Assert.Contains("d5a5b8c2-8d2f-4a3e-9b1c-7f6d5a4b3c2e", json.ToLower());
        }

        [Fact]
        public void AssetGuid_RoundTripSerialization()
        {
            var guid = new AssetGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var options = new JsonSerializerOptions();
            options.Converters.Add(new AssetGuid.JsonConverter());

            var json = JsonSerializer.Serialize(guid, options);
            var deserialized = JsonSerializer.Deserialize<AssetGuid>(json, options);
            Assert.Equal(guid, deserialized);
        }

        [Fact]
        public void ProjectGuid_EqualityWorks()
        {
            var guid1 = new ProjectGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var guid2 = new ProjectGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var guid3 = new ProjectGuid(new Guid("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF"));

            Assert.Equal(guid1, guid2);
            Assert.True(guid1 == guid2);
            Assert.NotEqual(guid1, guid3);
            Assert.True(guid1 != guid3);
        }

        [Fact]
        public void SceneGuid_EqualityWorks()
        {
            var guid1 = new SceneGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var guid2 = new SceneGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var guid3 = new SceneGuid(new Guid("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF"));

            Assert.Equal(guid1, guid2);
            Assert.True(guid1 == guid2);
            Assert.NotEqual(guid1, guid3);
            Assert.True(guid1 != guid3);
        }

        [Fact]
        public void EntityGuid_EqualityWorks()
        {
            var guid1 = new EntityGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var guid2 = new EntityGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var guid3 = new EntityGuid(new Guid("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF"));

            Assert.Equal(guid1, guid2);
            Assert.True(guid1 == guid2);
            Assert.NotEqual(guid1, guid3);
            Assert.True(guid1 != guid3);
        }

        [Fact]
        public void AssetGuid_EqualityWorks()
        {
            var guid1 = new AssetGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var guid2 = new AssetGuid(new Guid("D5A5B8C2-8D2F-4A3E-9B1C-7F6D5A4B3C2E"));
            var guid3 = new AssetGuid(new Guid("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF"));

            Assert.Equal(guid1, guid2);
            Assert.True(guid1 == guid2);
            Assert.NotEqual(guid1, guid3);
            Assert.True(guid1 != guid3);
        }
    }
}