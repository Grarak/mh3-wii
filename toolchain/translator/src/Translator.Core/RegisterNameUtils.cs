using System;

namespace Translator.Core;

internal static class RegisterNameUtils
{
    public static string StripNumericSuffix(string name)
    {
        if (string.IsNullOrEmpty(name))
        {
            return name;
        }

        var stripped = StripNumericSuffixSpan(name.AsSpan());
        // The span result is always a prefix of the input, so an unchanged
        // length means the original instance can be handed back untouched.
        return stripped.Length == name.Length ? name : new string(stripped);
    }

    /// <summary>
    /// Allocation-free form of <see cref="StripNumericSuffix(string)"/>. Not an overload on purpose:
    /// <c>StripNumericSuffix</c> is converted to a delegate as a method group elsewhere.
    /// </summary>
    public static ReadOnlySpan<char> StripNumericSuffixSpan(ReadOnlySpan<char> name)
    {
        if (name.IsEmpty)
        {
            return name;
        }

        var idx = name.LastIndexOf('_');
        if (idx <= 0)
        {
            return name;
        }

        for (var i = idx + 1; i < name.Length; i++)
        {
            if (!char.IsDigit(name[i]))
            {
                return name;
            }
        }

        return name[..idx];
    }

    public static string HardwareBase(string name)
    {
        if (string.IsNullOrEmpty(name))
        {
            return name;
        }

        var hardware = HardwareBase(name.AsSpan());
        return hardware.Length == name.Length ? name : new string(hardware);
    }

    public static ReadOnlySpan<char> HardwareBase(ReadOnlySpan<char> name)
    {
        var stripped = StripNumericSuffixSpan(name);
        var idx = stripped.IndexOf('_');
        return idx <= 0 ? stripped : stripped[..idx];
    }
}
