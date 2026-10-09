using System;
using System.Collections.Frozen;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;

namespace Translator.Core.CodeGen;

/// <summary>
/// Guest functions that touch hardware (MMIO registers, the GX FIFO, the EFB), configured by
/// <c>translation.hardware_access_path</c>: one <c>0xADDRESS</c> per line, <c>#</c> comments.
/// The runtime maps guest RAM flat and leaves hardware unmapped, so a plain access to hardware
/// faults. A listed function's accesses instead name <c>MemoryHw</c>, whose helpers route
/// hardware addresses to the device models: its emitted body has every
/// <c>MemoryInline::</c> helper and every paired-single helper (<c>PPC_Psq*</c>) requalified.
/// </summary>
public sealed class GuestHardwareAccess
{
    private static readonly Regex Line = new(@"^\s*0x([0-9A-Fa-f]{1,8})", RegexOptions.Compiled);
    private static readonly Regex PsqCall = new(@"(?<![\w:])PPC_Psq", RegexOptions.Compiled);
    private static readonly Regex Literal = new(@"(?<![\w.])-?\d+(?![\w.])", RegexOptions.Compiled);

    public static GuestHardwareAccess None { get; } = new(FrozenSet<uint>.Empty);

    /// <summary>Functions in effect for this process; <see cref="None"/> until a project sets them.</summary>
    public static GuestHardwareAccess Current { get; set; } = None;

    /// <summary>
    /// Functions that address guest RAM through another alias than cached MEM1/MEM2 (uncached,
    /// physical, the locked cache), configured by <c>translation.alias_access_path</c>. The
    /// runtime's default helpers (<c>MemoryInline</c>) address cached RAM with one add; a listed
    /// function's helpers are requalified to <c>MemoryAlias</c>, which folds every alias into the
    /// window. A function in both lists takes <c>MemoryHw</c>, which folds too.
    /// </summary>
    public static GuestHardwareAccess Alias { get; set; } = None;

    private readonly FrozenSet<uint> _functions;

    private GuestHardwareAccess(FrozenSet<uint> functions) => _functions = functions;

    public static GuestHardwareAccess Load(string path)
    {
        var functions = new HashSet<uint>();
        foreach (var line in File.ReadLines(path))
        {
            var match = Line.Match(line);
            if (match.Success)
                functions.Add(uint.Parse(match.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture));
        }
        return new GuestHardwareAccess(functions.ToFrozenSet());
    }

    public bool Contains(uint address) => _functions.Contains(address);

    /// <summary>
    /// Whether emitted code names an address of uncached MEM1/MEM2 (0xC0000000, 0xD0000000) or
    /// the locked cache (0xE0000000) as a constant: such a function takes <c>MemoryAlias</c>
    /// even when no run listed it (the SDK reads its globals through the uncached alias). The
    /// code generator writes constants it loads (lis, addi) in decimal and masks in hex, so only
    /// decimal literals count; one that only looks like an address costs the function the
    /// alias folding, nothing else.
    /// </summary>
    public static bool NamesAliasAddress(string code)
    {
        foreach (Match match in Literal.Matches(code))
        {
            if (!long.TryParse(match.Value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var number) || number < int.MinValue || number > uint.MaxValue)
                continue;

            var value = unchecked((uint)number);

            if (value - 0xC0000000u < 0x01800000u || value - 0xD0000000u < 0x04000000u || value - 0xE0000000u < 0x4000u)
                return true;
        }

        return false;
    }

    /// <summary>Requalifies a listed function's memory helpers to the hardware-aware family.</summary>
    public static string Rewrite(string code) => Requalify(code, "MemoryHw");

    /// <summary>Requalifies an alias-listed function's memory helpers to the folding family.</summary>
    public static string RewriteAlias(string code) => Requalify(code, "MemoryAlias");

    private static string Requalify(string code, string family) =>
        PsqCall.Replace(code.Replace("MemoryInline::", family + "::"), family + "::PPC_Psq");
}
