using Translator.Core.Mods;
using Xunit;

namespace Translator.Tests;

public class BaseFunctionIndexTests
{
    [Fact]
    public void FindContainingUsesHalfOpenRanges()
    {
        var index = new BaseFunctionIndex(
        [
            new BaseFunctionRangeMetadata(
                0x80533600,
                0x80533820,
                "func_80533600",
                "StaticR.rel:1",
                0x123600,
                "test",
                ["Executable"]),
            new BaseFunctionRangeMetadata(
                0x80533820,
                0x80533900,
                "func_80533820",
                "StaticR.rel:1",
                0x123820,
                "test",
                ["Executable"])
        ]);

        Assert.Null(index.FindContaining(0x805335FC));
        Assert.Equal(0x80533600u, index.FindContaining(0x80533600)!.Start);
        Assert.Equal(0x80533600u, index.FindContaining(0x8053381C)!.Start);
        Assert.Equal(0x80533820u, index.FindContaining(0x80533820)!.Start);
        Assert.Null(index.FindContaining(0x80533900));
    }
}
