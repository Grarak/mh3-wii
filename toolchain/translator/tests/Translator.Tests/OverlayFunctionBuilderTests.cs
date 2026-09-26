using System.Buffers.Binary;
using Translator.Core.Mods;
using Translator.Core.Parsing.Kamek;
using Xunit;

namespace Translator.Tests;

public class OverlayFunctionBuilderTests
{
    [Fact]
    public void BuildAndWriteAppliesModuleAndBaseOverlayPatches()
    {
        var tempRoot = Path.Combine(Path.GetTempPath(), "mkw_overlay_test_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(tempRoot);
        try
        {
            var baseDir = Path.Combine(tempRoot, "base");
            var outDir = Path.Combine(tempRoot, "out");
            Directory.CreateDirectory(baseDir);
            Directory.CreateDirectory(outDir);

            var baseImage = new byte[0x10];
            File.WriteAllBytes(Path.Combine(baseDir, "base_text.bin"), baseImage);

            const uint moduleBase = 0x81200000u;
            var chunk = new KamekChunk(
                0,
                0,
                0,
                0x10,
                0,
                0,
                0x30,
                new byte[0x10],
                [
                    new KamekCommand(0, 0, KamekCommandId.Write32, false, 0x4, [0xDEADBEEFu]),
                    new KamekCommand(4, 0, KamekCommandId.BranchLink, true, 0x80001004u, [0x100u]),
                    new KamekCommand(8, 0, KamekCommandId.Write32, true, 0x80001008u, [0x60000000u])
                ]);

            var manifest = new BaseManifest(
                "test",
                1,
                "RMCP01",
                "P",
                "",
                0,
                [
                    new BaseSectionMetadata(".text", "main.dol", 0x80001000u, 0x80001010u, true, false, "base_text.bin", 0)
                ],
                [
                    new BaseFunctionRangeMetadata(0x80001000u, 0x80001010u, "func_80001000", ".text", 0, "test", ["Executable"])
                ],
                "ranges.json");

            var plan = KamekPatchPlanner.Build(chunk, manifest, moduleBase);
            var result = OverlayFunctionBuilder.BuildAndWrite(chunk, manifest, plan, baseDir, outDir);

            var moduleImage = File.ReadAllBytes(Path.Combine(outDir, "overlay_images", result.ModuleImageFile));
            Assert.Equal(0xDEADBEEFu, BinaryPrimitives.ReadUInt32BigEndian(moduleImage.AsSpan(4, 4)));

            var overlay = Assert.Single(result.OverlayFunctions);
            var overlayImage = File.ReadAllBytes(Path.Combine(outDir, "overlay_images", overlay.ImageFile));
            Assert.Equal(
                KamekPpcEncoding.EncodeBranch(0x80001004u, moduleBase + 0x100u, link: true),
                BinaryPrimitives.ReadUInt32BigEndian(overlayImage.AsSpan(4, 4)));
            Assert.Equal(0x60000000u, BinaryPrimitives.ReadUInt32BigEndian(overlayImage.AsSpan(8, 4)));
            Assert.Equal(2, overlay.PatchCount);
            Assert.Empty(result.Diagnostics);
        }
        finally
        {
            if (Directory.Exists(tempRoot))
            {
                Directory.Delete(tempRoot, recursive: true);
            }
        }
    }
}
