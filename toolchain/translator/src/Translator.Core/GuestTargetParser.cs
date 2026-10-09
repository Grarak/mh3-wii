using System;
using System.Globalization;

namespace Translator.Core;

/// <summary>
/// The one place guest addresses are parsed out of text. Every spelling must be culture-invariant;
/// a culture-sensitive parse is what broke translation under tr-TR once already.
/// </summary>
public static class GuestTargetParser
{
    /// <summary>
    /// An IR call/jump target: <c>0x8000ABCD</c> or <c>func_8000ABCD</c>.
    /// Anything else - a runtime helper name, a native symbol - is deliberately
    /// not an address.
    /// </summary>
    public static bool TryParseAddress(string target, out uint address)
    {
        address = 0;
        if (string.IsNullOrWhiteSpace(target)) return false;

        var trimmed = target.Trim();
        var hex = trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? trimmed.AsSpan(2)
            : trimmed.StartsWith("func_", StringComparison.OrdinalIgnoreCase)
                ? trimmed.AsSpan(5)
                : default;
        return !hex.IsEmpty && TryParseHexDigits(hex, out address);
    }

    /// <summary>
    /// As <see cref="TryParseAddress"/>, but a target with neither prefix is read as a bare decimal,
    /// for the GQR propagation pass's synthetic edges spelled as plain numbers.
    /// </summary>
    public static bool TryParseAddressOrDecimal(string target, out uint address) =>
        TryParseAddress(target, out address) ||
        uint.TryParse(target, NumberStyles.Integer, CultureInfo.InvariantCulture, out address);

    /// <summary>
    /// A hexadecimal guest address whose <c>0x</c> prefix is optional: the
    /// spelling used by CLI options, generated file names, marker groups and
    /// JSON records.
    /// </summary>
    public static bool TryParseHexAddress(string text, out uint address)
    {
        address = 0;
        if (string.IsNullOrWhiteSpace(text)) return false;

        var hex = text.AsSpan().Trim();
        if (hex.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) hex = hex[2..];
        return TryParseHexDigits(hex, out address);
    }

    /// <summary>Throwing form of <see cref="TryParseHexAddress"/>, for command-line arguments.</summary>
    public static uint ParseHexAddress(string text) =>
        TryParseHexAddress(text, out var address)
            ? address
            : throw new FormatException($"'{text}' is not a hexadecimal guest address.");

    /// <summary>
    /// A generated local-label name: <c>loc_8000ABCD</c>, or the raw
    /// <c>0x8000ABCD</c> spelling the emitter uses before a label is named.
    /// Returns null for any other identifier.
    /// </summary>
    public static uint? TryParseLocalLabelAddress(string label)
    {
        if (string.IsNullOrWhiteSpace(label)) return null;

        var trimmed = label.Trim();
        var hex = trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? trimmed.AsSpan(2)
            : trimmed.StartsWith("loc_", StringComparison.OrdinalIgnoreCase)
                ? trimmed.AsSpan(4)
                : default;
        return !hex.IsEmpty && TryParseHexDigits(hex, out var address) ? address : null;
    }

    private static bool TryParseHexDigits(ReadOnlySpan<char> hex, out uint address) =>
        uint.TryParse(hex, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out address);
}
