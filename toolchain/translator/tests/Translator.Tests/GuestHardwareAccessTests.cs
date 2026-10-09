using Translator.Core.CodeGen;
using Xunit;

namespace Translator.Tests;

public class GuestHardwareAccessTests
{
    [Theory]
    [InlineData("r4 = -1073741824;\nr10 = MemoryInline::FlatRead32((r4 + 12612));", true)] // lis r4, 0xC000
    [InlineData("r3 = -805306368;", true)]                                                   // 0xD0000000
    [InlineData("r5 = -536870912;", true)]                                                   // the locked cache
    [InlineData("r6 = (r6 & 0xC0003FFFu);", false)]                                          // a mask, in hex
    [InlineData("r7 = -2147483648; r8 = 1127219200;", false)]                                // 0x80000000, 0x43300000
    [InlineData("r9 = -1048576000;", false)]                                                 // 0xC1800000, past uncached MEM1
    public void NamesAliasAddressFindsUncachedAndLockedCacheConstants(string code, bool expected)
    {
        Assert.Equal(expected, GuestHardwareAccess.NamesAliasAddress(code));
    }

    [Fact]
    public void RewriteAliasRequalifiesMemoryAndPairedSingleHelpers()
    {
        var code = "r3 = MemoryInline::FlatRead32(r4);\nf1 = PPC_PsqL(r5, 0, 0);";

        Assert.Equal("r3 = MemoryAlias::FlatRead32(r4);\nf1 = MemoryAlias::PPC_PsqL(r5, 0, 0);", GuestHardwareAccess.RewriteAlias(code));
        Assert.Equal("r3 = MemoryHw::FlatRead32(r4);\nf1 = MemoryHw::PPC_PsqL(r5, 0, 0);", GuestHardwareAccess.Rewrite(code));
    }
}
