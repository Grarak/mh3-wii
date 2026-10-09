using System.Collections.Frozen;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;

namespace Translator.Core.Lifting;

/// <summary>
/// Guest <c>lwz</c> instructions whose loaded word the runtime supplies: each lifts to
/// <c>MH3_HookedLoad32(base, offset, pc)</c> instead of a guest memory load. Configured by
/// <c>translation.load_hooks_path</c>, a file with one <c>{0xPC</c> or <c>0xPC</c> per line.
/// <see cref="TickSites"/> (<c>translation.tick_sites_path</c>, same format) lists loads of
/// step counters and countdowns: the loaded value passes through <c>MH3_TickLoad(value, pc)</c>.
/// </summary>
public sealed class GuestLoadHooks
{
    public const string Helper = "MH3_HookedLoad32";
    public const string TickHelper = "MH3_TickLoad";
    /// <summary>Float tick sites (<c>EaseDivisor</c> kind): the divisor d of an ease
    /// <c>x += (goal - x) / d</c>, rescaled for the step's length.</summary>
    public const string EaseDivisorHelper = "MH3_EaseDivisor";
    /// <summary>Float tick sites (<c>PerStep</c> kind): an amount applied once per step,
    /// scaled by the step's length.</summary>
    public const string PerStepHelper = "MH3_PerStep";
    /// <summary>Float tick sites (<c>Decay</c> kind): a factor <c>x *= f</c> applied once per
    /// step, raised to the step's length.</summary>
    public const string DecayHelper = "MH3_Decay";
    private static readonly Regex Line = new(@"^\s*\{?\s*0x([0-9A-Fa-f]{1,8})", RegexOptions.Compiled);

    public static GuestLoadHooks None { get; } = new(FrozenSet<uint>.Empty, FrozenSet<uint>.Empty, FrozenSet<uint>.Empty, FrozenSet<uint>.Empty);

    /// <summary>Hooks in effect for this process; <see cref="None"/> until a project sets them.</summary>
    public static GuestLoadHooks Current { get; set; } = None;

    /// <summary>Tick sites in effect for this process.</summary>
    public static GuestLoadHooks TickSites { get; set; } = None;

    private readonly FrozenSet<uint> _addresses;
    private readonly FrozenSet<uint> _easeDivisors;
    private readonly FrozenSet<uint> _perStep;
    private readonly FrozenSet<uint> _decay;

    private GuestLoadHooks(FrozenSet<uint> addresses, FrozenSet<uint> easeDivisors, FrozenSet<uint> perStep,
        FrozenSet<uint> decay)
    {
        _addresses = addresses;
        _easeDivisors = easeDivisors;
        _perStep = perStep;
        _decay = decay;
    }

    public static GuestLoadHooks Load(string path)
    {
        var addresses = new HashSet<uint>();
        var easeDivisors = new HashSet<uint>();
        var perStep = new HashSet<uint>();
        var decay = new HashSet<uint>();
        foreach (var line in File.ReadLines(path))
        {
            var match = Line.Match(line);
            if (!match.Success)
                continue;
            var address = uint.Parse(match.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            addresses.Add(address);
            if (line.Contains("EaseDivisor", System.StringComparison.Ordinal))
                easeDivisors.Add(address);
            if (line.Contains("PerStep", System.StringComparison.Ordinal))
                perStep.Add(address);
            if (line.Contains("TickSite::Decay", System.StringComparison.Ordinal))
                decay.Add(address);
        }
        return new GuestLoadHooks(addresses.ToFrozenSet(), easeDivisors.ToFrozenSet(), perStep.ToFrozenSet(),
            decay.ToFrozenSet());
    }

    public bool Contains(uint address) => _addresses.Contains(address);

    /// <summary>A float load site of the <c>EaseDivisor</c> kind.</summary>
    public bool IsEaseDivisor(uint address) => _easeDivisors.Contains(address);

    /// <summary>The helper a float load site's value passes through, or null for none.</summary>
    public string? FloatHelper(uint address) =>
        _easeDivisors.Contains(address) ? EaseDivisorHelper
        : _perStep.Contains(address) ? PerStepHelper
        : _decay.Contains(address) ? DecayHelper
        : null;
}
