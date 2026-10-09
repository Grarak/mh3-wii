using System.Buffers.Binary;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Mods;
using Translator.Core.Mods.Mkwii;
using Translator.Core.Parsing.Kamek;
using Xunit;

namespace Translator.Tests;

public class ContinuationPlannerTests
{
    [Fact]
    public void AddModuleTailJumpContinuationsAddsInternalBaseTarget()
    {
        const uint moduleBase = 0x81200000u;
        var moduleImage = new byte[0x20];
        WriteU32(moduleImage, 0x00, 0x3D80807Eu); // lis r12,0x807E
        WriteU32(moduleImage, 0x04, 0x618C3064u); // ori r12,r12,0x3064
        WriteU32(moduleImage, 0x08, 0x7D8903A6u); // mtctr r12
        WriteU32(moduleImage, 0x0C, 0x4E800420u); // bctr

        var plan = ContinuationPlanner.Build(EmptyChunk(), TestManifest(), moduleBase);
        var result = ContinuationPlanner.AddModuleTailJumpContinuations(
            plan,
            TestManifest(),
            moduleBase,
            moduleImage);

        var entry = Assert.Single(result.Entries);
        Assert.Equal(0x807E3064u, entry.Address);
        Assert.Equal(0x807E2D18u, entry.ContainingFunctionStart);
        Assert.Equal(0x807E30ACu, entry.ContainingFunctionEnd);
        Assert.Equal(0x8120000Cu, entry.SourceCommandAddress);
        Assert.Contains("tail jump", entry.Reason);
    }

    [Fact]
    public void AddModuleTailJumpContinuationsSupportsSignedAddiLowImmediate()
    {
        const uint moduleBase = 0x81200000u;
        var moduleImage = new byte[0x20];
        WriteU32(moduleImage, 0x00, 0x3D80807Fu); // lis r12,0x807F
        WriteU32(moduleImage, 0x04, 0x398CFFF0u); // addi r12,r12,-0x10
        WriteU32(moduleImage, 0x08, 0x7D8903A6u); // mtctr r12
        WriteU32(moduleImage, 0x0C, 0x4E800420u); // bctr

        var manifest = new BaseManifest(
            "test",
            1,
            "RMCP01",
            "P",
            "",
            0,
            [
                new BaseSectionMetadata(".text", "StaticR.rel", 0x807EFFF0u, 0x807F0100u, true, false, "base_text.bin", 0)
            ],
            [
                new BaseFunctionRangeMetadata(0x807EFFE0u, 0x807F0100u, "func_807EFFE0", ".text", 0, "test", ["Executable"])
            ],
            "ranges.json");

        var plan = ContinuationPlanner.Build(EmptyChunk(), manifest, moduleBase);
        var result = ContinuationPlanner.AddModuleTailJumpContinuations(plan, manifest, moduleBase, moduleImage);

        var entry = Assert.Single(result.Entries);
        Assert.Equal(0x807EFFF0u, entry.Address);
        Assert.Equal(0x807EFFE0u, entry.ContainingFunctionStart);
    }

    [Fact]
    public void AddModuleTailJumpContinuationsIgnoresFunctionEntries()
    {
        const uint moduleBase = 0x81200000u;
        var moduleImage = new byte[0x20];
        WriteU32(moduleImage, 0x00, 0x3D80807Eu); // lis r12,0x807E
        WriteU32(moduleImage, 0x04, 0x618C2D18u); // ori r12,r12,0x2D18
        WriteU32(moduleImage, 0x08, 0x7D8903A6u); // mtctr r12
        WriteU32(moduleImage, 0x0C, 0x4E800420u); // bctr

        var manifest = TestManifest();
        var plan = ContinuationPlanner.Build(EmptyChunk(), manifest, moduleBase);
        var result = ContinuationPlanner.AddModuleTailJumpContinuations(plan, manifest, moduleBase, moduleImage);

        Assert.Empty(result.Entries);
    }

    [Fact]
    public void AddRetroWfcExecutableHookContinuationsAddsInternalBaseTarget()
    {
        var manifest = new BaseManifest(
            "test",
            1,
            "RMCP01",
            "P",
            "",
            0,
            [
                new BaseSectionMetadata(".text", "main.dol", 0x800F164Cu, 0x800F1BC8u, true, false, "base_text.bin", 0)
            ],
            [
                new BaseFunctionRangeMetadata(0x800F164Cu, 0x800F1BC8u, "func_800F164C", ".text", 0, "test", ["Executable"])
            ],
            "ranges.json");
        var hook = new RetroWfcExecutableHookPlan(
            5,
            "executableHookWithContinuation",
            "branchCtr",
            0x800F164Cu,
            4,
            0x800F164Cu,
            0x800F1BC8u,
            ".text",
            ["support.hostnameRewrite.gethostbyname"],
            "overlayHookPatch",
            0x8179E1A4u,
            "support.hostnameRewrite.gethostbyname",
            "moduleFunction",
            null,
            0x800F165Cu);

        var plan = ContinuationPlanner.Build(EmptyChunk(), manifest, 0x81200000u);
        var result = ContinuationPlanner.AddRetroWfcExecutableHookContinuations(plan, manifest, [hook]);

        var entry = Assert.Single(result.Entries);
        Assert.Equal(0x800F165Cu, entry.Address);
        Assert.Equal(0x800F164Cu, entry.ContainingFunctionStart);
        Assert.Equal(0x800F1BC8u, entry.ContainingFunctionEnd);
        Assert.Equal(0x800F164Cu, entry.SourceCommandAddress);
        Assert.Contains("Retro WFC executable hook continuation", entry.Reason);
    }

    [Fact]
    public void DiscoverLrRelativeIndirectJumpOffsets_DiscoversSkipReturnOffset()
    {
        var instructions = new[]
        {
            PpcDecoder.Decode(0x8180D8E8, 0x7FE802A6u), // mflr r31
            PpcDecoder.Decode(0x8180D8EC, 0x3BFF0014u), // addi r31, r31, 20
            PpcDecoder.Decode(0x8180D8F0, 0x7FE803A6u), // mtlr r31
            PpcDecoder.Decode(0x8180D8F4, 0x4E800020u), // blr
        };

        var offsets = ContinuationPlanner.DiscoverLrRelativeIndirectJumpOffsets(instructions).ToArray();
        var offset = Assert.Single(offsets);
        Assert.Equal(20, offset);
    }

    [Fact]
    public void DiscoverLrRelativeIndirectJumpOffsets_IgnoresStandardLrRestore()
    {
        var instructions = new[]
        {
            PpcDecoder.Decode(0x8180D8E8, 0x7FE802A6u), // mflr r31
            PpcDecoder.Decode(0x8180D8EC, 0x93E10008u), // stw r31, 8(r1)
            PpcDecoder.Decode(0x8180D8F0, 0x83E10008u), // lwz r31, 8(r1)
            PpcDecoder.Decode(0x8180D8F4, 0x7FE803A6u), // mtlr r31
            PpcDecoder.Decode(0x8180D8F8, 0x4E800020u), // blr
        };

        var offsets = ContinuationPlanner.DiscoverLrRelativeIndirectJumpOffsets(instructions);
        Assert.Empty(offsets);
    }

    [Fact]
    public void DiscoverLrRelativeIndirectJumpOffsets_SupportsBctrOffset()
    {
        var instructions = new[]
        {
            PpcDecoder.Decode(0x8180D8E8, 0x7FE802A6u), // mflr r31
            PpcDecoder.Decode(0x8180D8EC, 0x397F0008u), // addi r11, r31, 8
            PpcDecoder.Decode(0x8180D8F0, 0x7D6903A6u), // mtctr r11
            PpcDecoder.Decode(0x8180D8F4, 0x4E800420u), // bctr
        };

        var offsets = ContinuationPlanner.DiscoverLrRelativeIndirectJumpOffsets(instructions).ToArray();
        var offset = Assert.Single(offsets);
        Assert.Equal(8, offset);
    }

    private static KamekChunk EmptyChunk() =>
        new(
            0,
            0,
            0,
            0,
            0,
            0,
            KamekChunk.HeaderSize,
            [],
            []);

    private static BaseManifest TestManifest() =>
        new(
            "test",
            1,
            "RMCP01",
            "P",
            "",
            0,
            [
                new BaseSectionMetadata(".text", "StaticR.rel", 0x807E2D18u, 0x807E30ACu, true, false, "base_text.bin", 0)
            ],
            [
                new BaseFunctionRangeMetadata(0x807E2D18u, 0x807E30ACu, "func_807E2D18", ".text", 0, "test", ["Executable"])
            ],
            "ranges.json");

    private static void WriteU32(byte[] data, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(offset, 4), value);
}
