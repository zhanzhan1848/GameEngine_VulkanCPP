using System;
using PrimalEditor_Avalonia.Content;
using System.Text.Json;
using Xunit;

namespace PrimalEditor_Avalonia.Tests.Content
{
    public class ProjectRelativePathTests
    {
        [Fact]
        public void Constructor_RejectsRootedPaths()
        {
            Assert.Throws<ArgumentException>(() => new ProjectRelativePath("/absolute/path"));
            Assert.Throws<ArgumentException>(() => new ProjectRelativePath("C:\\absolute\\path"));
        }

        [Fact]
        public void Constructor_RejectsDoubleDotTraversal()
        {
            Assert.Throws<ArgumentException>(() => new ProjectRelativePath("../escape"));
            Assert.Throws<ArgumentException>(() => new ProjectRelativePath("valid/../escape"));
        }

        [Fact]
        public void Constructor_RejectsEmptySegments()
        {
            Assert.Throws<ArgumentException>(() => new ProjectRelativePath(""));
            Assert.Throws<ArgumentException>(() => new ProjectRelativePath("valid//double"));
        }

        [Fact]
        public void Constructor_RejectsCurrentDirectorySegments()
        {
            Assert.Throws<ArgumentException>(() => new ProjectRelativePath("./hidden"));
            Assert.Throws<ArgumentException>(() => new ProjectRelativePath("valid/./hidden"));
        }

        [Fact]
        public void Constructor_AllowsDoubleDotsInsideAFileName()
        {
            var path = new ProjectRelativePath("folder/file..backup");
            Assert.Equal("folder/file..backup", path.Value);
        }

        [Fact]
        public void Resolve_DoesNotTreatSiblingPrefixAsInsideRoot()
        {
            var path = new ProjectRelativePath("asset.txt");
            var root = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "project");
            var resolved = path.Resolve(root);

            Assert.Equal(System.IO.Path.Combine(root, "asset.txt"), resolved);
        }

        [Fact]
        public void Constructor_NormalizesSeparatorsToForwardSlash()
        {
            var path = new ProjectRelativePath("folder\\subfolder\\file.txt");
            Assert.Equal("folder/subfolder/file.txt", path.Value);
        }

        [Fact]
        public void Constructor_ResolvesWithProjectRoot()
        {
            var path = new ProjectRelativePath("folder/file.txt");
            var resolved = path.Resolve("/project/root");
            Assert.Equal("/project/root/folder/file.txt", resolved);
        }

        [Fact]
        public void Resolve_ResolvesCorrectlyAgainstProjectRoot()
        {
            var path = new ProjectRelativePath("folder/file.txt");
            var resolved = path.Resolve("/project/root");
            Assert.Contains("folder/file.txt", resolved);
        }

        [Fact]
        public void Resolve_RejectsSiblingDirectoryThatOnlySharesRootPrefix()
        {
            var rootParent = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "PrimalPathRoot");
            var root = System.IO.Path.Combine(rootParent, "project");
            var siblingFile = System.IO.Path.Combine(rootParent, "project-other", "asset.txt");
            var relative = System.IO.Path.GetRelativePath(root, siblingFile);

            Assert.Throws<ArgumentException>(() => new ProjectRelativePath(relative));
        }

        [Fact]
        public void Value_ReturnsOriginalNormalizedPath()
        {
            var path = new ProjectRelativePath("folder\\subfolder\\file.txt");
            Assert.Equal("folder/subfolder/file.txt", path.Value);
        }

        [Fact]
        public void Equality_SamePathsAreEqual()
        {
            var path1 = new ProjectRelativePath("folder/file.txt");
            var path2 = new ProjectRelativePath("folder\\file.txt");
            Assert.Equal(path1, path2);
            Assert.True(path1 == path2);
        }

        [Fact]
        public void Equality_DifferentPathsAreNotEqual()
        {
            var path1 = new ProjectRelativePath("folder/file.txt");
            var path2 = new ProjectRelativePath("other/file.txt");
            Assert.NotEqual(path1, path2);
            Assert.True(path1 != path2);
        }

        [Fact]
        public void JsonSerialization_RoundTrip()
        {
            var options = new JsonSerializerOptions();
            options.Converters.Add(new ProjectRelativePath.JsonConverter());

            var path = new ProjectRelativePath("folder/file.txt");
            var json = JsonSerializer.Serialize(path, options);
            var deserialized = JsonSerializer.Deserialize<ProjectRelativePath>(json, options);
            Assert.Equal(path, deserialized);
        }

        [Fact]
        public void UnixPath_WorksCorrectly()
        {
            var path = new ProjectRelativePath("folder/subfolder/file.txt");
            Assert.Equal("folder/subfolder/file.txt", path.Value);
            var resolved = path.Resolve("/project/root");
            Assert.Equal("/project/root/folder/subfolder/file.txt", resolved);
        }

        [Fact]
        public void WindowsPath_WorksCorrectly()
        {
            var path = new ProjectRelativePath("folder\\subfolder\\file.txt");
            Assert.Equal("folder/subfolder/file.txt", path.Value);
            var resolved = path.Resolve("C:\\project\\root");
            Assert.Contains("folder/subfolder/file.txt", resolved);
        }
    }
}