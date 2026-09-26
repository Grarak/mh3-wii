using System.IO;
using System.Linq;
using System.Buffers.Binary;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;
using Xunit;

namespace Translator.Tests;

public class DolFileTests
{
    private static string DolPath => Path.Combine(ProjectPaths.FindRepositoryRoot(), "assets", "main.dol");

    [Fact]
    public void ParsesExpectedSectionsAndRanges()
    {
        var dol = DolFile.Load(DolPath);

        Assert.Equal(0x800060A4u, dol.EntryPoint);
        Assert.Equal(0x80004000u, dol.MemoryRange.Start);
        Assert.Equal(0x8038917Cu, dol.MemoryRange.End);

        Assert.Equal(11, dol.Sections.Count);
        Assert.Equal(2, dol.Sections.Count(s => s.Kind == SectionKind.Text));
        Assert.Equal(8, dol.Sections.Count(s => s.Kind == SectionKind.Data));
        Assert.Equal(1, dol.Sections.Count(s => s.Kind == SectionKind.Bss));

        var init = dol.Sections.Single(s => s.Name == ".init");
        Assert.Equal(0x80004000u, init.VirtualAddress);
        Assert.Equal(0x2460u, init.Size);

        var text = dol.Sections.Single(s => s.Name == ".text");
        Assert.Equal(0x800072C0u, text.VirtualAddress);
        Assert.Equal(0x23DB20u, text.Size);

        var data = dol.Sections.Single(s => s.Name == ".data");
        Assert.Equal(0x80258580u, data.VirtualAddress);
        Assert.Equal(0x4BAC0u, data.Size);

        var bss = dol.Sections.Single(s => s.Kind == SectionKind.Bss);
        Assert.Equal(0x802A4080u, bss.VirtualAddress);
        Assert.Equal(0xE50FCu, bss.Size);
    }

    [Fact]
    public void MkwOutlineEffectConstantLivesInInitializedSdata2DespiteDolHeaderBssOverlap()
    {
        var dol = DolFile.Load(DolPath);
        var sdata2 = dol.Sections.Single(s => s.Name == ".sdata2");
        const uint outlineEffectScaleAddress = 0x80389060;

        Assert.True(sdata2.Range.Contains(outlineEffectScaleAddress));
        Assert.True(dol.Sections.Single(s => s.Kind == SectionKind.Bss).Range.Contains(outlineEffectScaleAddress));

        var offset = checked((int)(outlineEffectScaleAddress - sdata2.VirtualAddress));
        var rawValue = BinaryPrimitives.ReadUInt32BigEndian(sdata2.Data.Span.Slice(offset, sizeof(uint)));

        Assert.Equal(0x41200000u, rawValue);
    }
}
