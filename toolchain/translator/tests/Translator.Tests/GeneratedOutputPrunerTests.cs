using Translator.Core.Build;
using Translator.Core.Mods;

namespace Translator.Tests;

public sealed class GeneratedOutputPrunerTests
{
    [Fact]
    public void MetadataPruningRemovesStaleTranslatedBody()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkw_prune_test", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            var currentCpp = Path.Combine(root, "current.cpp");
            var staleCpp = Path.Combine(root, "stale.cpp");
            File.WriteAllText(currentCpp, "// current");
            File.WriteAllText(staleCpp, "// stale");

            var metadata = BaseTranslationOutputMetadata.Create([
                FunctionMetadata("current.cpp", 0x80001000),
                FunctionMetadata("stale.cpp", 0x80001004)
            ], TranslationQualityMetadata.Clean);
            var emitted = new HashSet<string>(
                [Path.GetFullPath(currentCpp)],
                OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);

            var result = GeneratedOutputPruner.Prune(root, emitted, metadata);

            Assert.Equal(1, result.RemovedFiles);
            Assert.Empty(result.Warnings);
            Assert.True(File.Exists(currentCpp));
            Assert.False(File.Exists(staleCpp));
        }
        finally
        {
            if (Directory.Exists(root))
                Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void MetadataPruningRefusesPathOutsideOutputRoot()
    {
        var parent = Path.Combine(Path.GetTempPath(), "mkw_prune_test", Guid.NewGuid().ToString("N"));
        var root = Path.Combine(parent, "generated");
        Directory.CreateDirectory(root);
        var outside = Path.Combine(parent, "outside.cpp");
        File.WriteAllText(outside, "// user owned");
        try
        {
            var metadata = BaseTranslationOutputMetadata.Create([
                FunctionMetadata("../outside.cpp", 0x80001000)
            ], TranslationQualityMetadata.Clean);

            var result = GeneratedOutputPruner.Prune(root, new HashSet<string>(), metadata);

            Assert.Equal(0, result.RemovedFiles);
            Assert.Contains(result.Warnings, warning => warning.Contains("outside output root", StringComparison.Ordinal));
            Assert.True(File.Exists(outside));
        }
        finally
        {
            if (Directory.Exists(parent))
                Directory.Delete(parent, recursive: true);
        }
    }

    private static BaseTranslationFunctionMetadata FunctionMetadata(
        string relativePath,
        uint address)
    {
        var build = new BaseTranslationFunctionBuildMetadata(
            $"func_{address:X8}",
            true,
            0,
            [],
            null,
            1);
        return new BaseTranslationFunctionMetadata(
            relativePath,
            1,
            new string('b', 64),
            address,
            [],
            build);
    }
}
