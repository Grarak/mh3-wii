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

    public static GuestHardwareAccess None { get; } = new(FrozenSet<uint>.Empty);

    /// <summary>Functions in effect for this process; <see cref="None"/> until a project sets them.</summary>
    public static GuestHardwareAccess Current { get; set; } = None;

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

    /// <summary>Requalifies a listed function's memory helpers to the hardware-aware family.</summary>
    public static string Rewrite(string code) =>
        PsqCall.Replace(code.Replace("MemoryInline::", "MemoryHw::"), "MemoryHw::PPC_Psq");
}
