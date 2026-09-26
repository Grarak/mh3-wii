using Translator.Core;
using Translator.Core.Analysis;
using Xunit;

namespace Translator.Tests;

/// <summary>
/// Proves marker text and its read-back patterns stay in sync: every marker the emitter writes
/// must be matched, group for group, by the pattern consumers parse it with.
/// </summary>
public sealed class GeneratedMarkersTests
{
    [Fact]
    public void GuestAbiMarkerRoundTripsThroughItsPattern()
    {
        var contract = new GuestAbiContract(
            GprReadBeforeWriteMask: 0x00000018u,
            GprPossibleWriteMask: 0x0000000Cu,
            GprReturnMask: 0x00000008u,
            FprReadBeforeWriteMask: 0x00000002u,
            FprPossibleWriteMask: 0x00000006u,
            FprReturnMask: 0x00000002u,
            CrReadBeforeWriteMask: 0x12,
            CrPossibleWriteMask: 0x34,
            ReadsXerBeforeWrite: true,
            MayWriteXer: false,
            ReadsCtrBeforeWrite: false,
            MayWriteCtr: false,
            ReadsLrBeforeWrite: false,
            MayWriteLr: false,
            BoundaryFlags: GuestCallBoundaryFlags.None,
            DirectCallTargets: Array.Empty<uint>());

        var marker = GeneratedMarkers.GuestAbi(contract);
        var match = GeneratedMarkers.GuestAbiPattern().Match(marker);

        Assert.True(match.Success, marker);
        Assert.Equal("00000018", match.Groups["gr"].Value);
        Assert.Equal("0000000C", match.Groups["gw"].Value);
        Assert.Equal("00000008", match.Groups["gret"].Value);
        Assert.Equal("00000002", match.Groups["fr"].Value);
        Assert.Equal("00000006", match.Groups["fw"].Value);
        Assert.Equal("00000002", match.Groups["fret"].Value);
        Assert.Equal("12", match.Groups["crr"].Value);
        Assert.Equal("34", match.Groups["crw"].Value);
        Assert.Equal("1", match.Groups["xr"].Value);
        Assert.Equal("0", match.Groups["xw"].Value);
        Assert.Equal(contract.HasFullSynchronizationFence ? "1" : "0", match.Groups["fence"].Value);
    }

    [Fact]
    public void BaseRegistrationMarkerRoundTripsThroughItsPattern()
    {
        var marker = GeneratedMarkers.BaseRegistration(0x80001234u, "func_80001234", true, 0xFC000000u);

        Assert.Equal(
            "// RECOMP_REGISTRATION base 0x80001234 func_80001234 preserves=true fpr_mask=0xFC000000",
            marker);

        var match = GeneratedMarkers.BaseRegistrationPattern().Match(marker);
        Assert.True(match.Success);
        Assert.Equal("80001234", match.Groups["address"].Value);
        Assert.Equal("func_80001234", match.Groups["symbol"].Value);
        Assert.Equal("true", match.Groups["preserves"].Value);
        Assert.Equal("FC000000", match.Groups["mask"].Value);
        Assert.Matches(GeneratedMarkers.RegistrationLinePattern(), marker + "\n");
    }

    [Fact]
    public void ModRegistrationMarkerRoundTripsThroughItsPattern()
    {
        var marker = GeneratedMarkers.ModRegistration(
            0x80002000u, "rr_80002000", "Pretty::Name", false, 0x00004000u, 100u, 7ul);

        Assert.Equal(
            "// RECOMP_REGISTRATION mod 0x80002000 rr_80002000 \"Pretty::Name\" " +
            "preserves=false fpr_mask=0x00004000 priority=100 module_id=7",
            marker);

        var match = GeneratedMarkers.ModRegistrationPattern().Match(marker);
        Assert.True(match.Success);
        Assert.Equal("80002000", match.Groups["address"].Value);
        Assert.Equal("rr_80002000", match.Groups["symbol"].Value);
        Assert.Equal("Pretty::Name", match.Groups["name"].Value);
        Assert.Equal("false", match.Groups["preserves"].Value);
        Assert.Equal("00004000", match.Groups["mask"].Value);
        Assert.Equal("100", match.Groups["priority"].Value);
        Assert.Equal("7", match.Groups["moduleId"].Value);
    }
}
