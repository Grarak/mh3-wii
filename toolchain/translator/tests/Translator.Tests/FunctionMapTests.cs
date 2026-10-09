using System.IO;
using Translator.Core.Analysis;
using Translator.Core.Loading;
using Xunit;

namespace Translator.Tests;

public sealed class FunctionMapTests
{
    private static FunctionMap Parse(params string[] lines) => FunctionMap.Parse(lines, "test-map");

    [Fact]
    public void ParsesNamedAndUnnamedEntriesInAscendingOrder()
    {
        var map = Parse(
            "80006210 __init_registers",
            "800018a8 0x800018a8",
            "",
            "# comment",
            "800060a4 __start",
            "800018a8 0x800018a8");

        Assert.Equal(new uint[] { 0x800018A8u, 0x800060A4u, 0x80006210u }, map.Addresses);
        Assert.Equal(3, map.Addresses.Count);
        Assert.Equal(2, map.NamedCount);
        Assert.True(map.Contains(0x800060A4u));
        Assert.False(map.Contains(0x800060A8u));
    }

    [Fact]
    public void LooksUpBothWaysAndTreatsRepeatedAddressPlaceholdersAsUnnamed()
    {
        var map = Parse(
            "800018a8 0x800018a8",
            "800060a4 __start");

        Assert.Equal("__start", map.NameOf(0x800060A4u));
        Assert.Null(map.NameOf(0x800018A8u));
        Assert.Null(map.NameOf(0x80009999u));

        Assert.True(map.TryGetAddress("__start", out var start));
        Assert.Equal(0x800060A4u, start);
        Assert.False(map.TryGetAddress("0x800018a8", out _));
    }

    [Fact]
    public void RejectsNonHexAndEmptyMaps()
    {
        Assert.Throws<InvalidDataException>(() => Parse("not_an_address foo"));
        Assert.Throws<InvalidDataException>(() => Parse("# only a comment"));
    }

    [Fact]
    public void RecoversSaveRestoreThunkRangesFromNamedFamilies()
    {
        var map = Parse(
            "800214f8 _save_fpr_23",
            "8002150c _save_fpr_28",
            "80021544 _rest_fpr_23",
            "80021558 _rest_fpr_28",
            "8002156c _save_gpr_14",
            "80021570 _save_gpr_15",
            "800215a0 _save_gpr_27",
            "800215b8 _rest_gpr_14",
            "800215ec _rest_gpr_27");

        var thunks = GuestSaveRestoreThunks.FromFunctionMap(map);

        Assert.Equal(new GuestSaveRestoreThunkRange(0x8002156Cu, 14, 27), thunks.SaveGpr);
        Assert.Equal(new GuestSaveRestoreThunkRange(0x800215B8u, 14, 27), thunks.RestGpr);
        Assert.Equal(new GuestSaveRestoreThunkRange(0x800214F8u, 23, 28), thunks.SaveFpr);
        Assert.Equal(new GuestSaveRestoreThunkRange(0x80021544u, 23, 28), thunks.RestFpr);
        Assert.False(thunks.IsEmpty);
    }

    [Fact]
    public void ThunkRecoveryRejectsRunsThatAreNotFourBytesPerRegister()
    {
        var map = Parse(
            "8002156c _save_gpr_14",
            "800215a4 _save_gpr_27"); // one slot too far

        Assert.Throws<InvalidDataException>(() => GuestSaveRestoreThunks.FromFunctionMap(map));
    }

    [Fact]
    public void MapWithoutThunkSymbolsYieldsNoRanges()
    {
        var thunks = GuestSaveRestoreThunks.FromFunctionMap(Parse("800060a4 __start"));

        Assert.True(thunks.IsEmpty);
        Assert.Null(thunks.SaveGpr);
    }
}
