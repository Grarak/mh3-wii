using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Translator.Core.Loading;

namespace Translator.Core.Analysis;

/// <summary>
/// One CodeWarrior save/restore thunk family: a run of one-instruction entry
/// points, four bytes apart, where entry <c>k</c> saves or restores registers
/// <c>FirstRegister + k</c> upwards.
/// </summary>
public sealed record GuestSaveRestoreThunkRange(uint BaseAddress, int FirstRegister, int LastRegister);

/// <summary>
/// The four CodeWarrior save/restore thunk ranges, derived once from the function map's symbol names
/// (<c>_save_gpr_14</c>, <c>_rest_gpr_14</c>, <c>_save_fpr_23</c>, <c>_rest_fpr_23</c> ...) and published
/// here instead of threaded as a parameter through the three passes that recognize these thunks.
/// </summary>
public sealed class GuestSaveRestoreThunks
{
    public static GuestSaveRestoreThunks None { get; } = new(null, null, null, null);

    /// <summary>
    /// Ranges in effect for this process. Set once from the loaded project;
    /// <see cref="None"/> until then, which makes every thunk query answer "no",
    /// exactly as it does for a project that configures no function map.
    /// </summary>
    public static GuestSaveRestoreThunks Current { get; set; } = None;

    private GuestSaveRestoreThunks(
        GuestSaveRestoreThunkRange? saveGpr,
        GuestSaveRestoreThunkRange? restGpr,
        GuestSaveRestoreThunkRange? saveFpr,
        GuestSaveRestoreThunkRange? restFpr)
    {
        SaveGpr = saveGpr;
        RestGpr = restGpr;
        SaveFpr = saveFpr;
        RestFpr = restFpr;
    }

    public GuestSaveRestoreThunkRange? SaveGpr { get; }
    public GuestSaveRestoreThunkRange? RestGpr { get; }
    public GuestSaveRestoreThunkRange? SaveFpr { get; }
    public GuestSaveRestoreThunkRange? RestFpr { get; }

    public bool IsEmpty => SaveGpr is null && RestGpr is null && SaveFpr is null && RestFpr is null;

    public static GuestSaveRestoreThunks FromFunctionMap(FunctionMap map) =>
        new(
            ResolveRange(map, "_save_gpr_"),
            ResolveRange(map, "_rest_gpr_"),
            ResolveRange(map, "_save_fpr_"),
            ResolveRange(map, "_rest_fpr_"));

    /// <summary>
    /// Recovers (base, first, last register) from the family's named entries, validating that the
    /// run is exactly four bytes per register step since only the family's extremes are guaranteed named.
    /// </summary>
    private static GuestSaveRestoreThunkRange? ResolveRange(FunctionMap map, string prefix)
    {
        var family = map.NumberedFamily(prefix);
        if (family.Count < 2) return null;

        var first = family.Keys.Min();
        var last = family.Keys.Max();
        var baseAddress = family[first];
        var expectedEnd = baseAddress + checked((uint)((last - first) * 4));
        if (family[last] != expectedEnd)
        {
            throw new InvalidDataException(
                $"Function map {map.SourcePath} describes '{prefix}' thunks at 0x{baseAddress:X8}..0x{family[last]:X8} " +
                $"for registers {first}..{last}, which is not a four-byte-per-register run.");
        }

        foreach (var (register, address) in family)
        {
            var expected = baseAddress + checked((uint)((register - first) * 4));
            if (address != expected)
            {
                throw new InvalidDataException(
                    $"Function map {map.SourcePath} places '{prefix}{register}' at 0x{address:X8}; " +
                    $"the surrounding run requires 0x{expected:X8}.");
            }
        }

        return new GuestSaveRestoreThunkRange(baseAddress, first, last);
    }
}
