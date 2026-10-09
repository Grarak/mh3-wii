using System;
using System.Collections.Generic;
using System.Globalization;
using Translator.Core.Ir;
using Translator.Core.Analysis.Representation;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// Machine register classes recognized by the emitter. Exactly one of these
    /// applies to any base register name, which is what lets the individual
    /// predicates share a single allocation-free classification pass.
    /// </summary>
    private enum GuestRegisterClass
    {
        None = 0,
        Gpr,
        Fpr,
        CrField,
        Special
    }

    /// <summary>Canonical <c>r0</c>..<c>r31</c> / <c>f0</c>..<c>f31</c> spellings, interned so the
    /// dataflow passes and residency emitter stop reformatting them per instruction per iteration.</summary>
    private static readonly string[] GprRegisterNames = CreateRegisterNames("r");
    private static readonly string[] FprRegisterNames = CreateRegisterNames("f");

    /// <summary>Scalar (PS0) spelling of a resident FPR local: <c>f0.d</c>..<c>f31.d</c>.</summary>
    private static readonly string[] FprScalarLocalNames = CreateFprScalarLocalNames();

    private static string[] CreateFprScalarLocalNames()
    {
        var names = new string[32];
        for (var index = 0; index < names.Length; index++)
        {
            names[index] = "f" + index.ToString(CultureInfo.InvariantCulture) + ".d";
        }

        return names;
    }

    /// <summary>Architectural <c>CpuContext</c> spellings used when no residency is active.</summary>
    private static readonly string[] CtxGprNames = CreateCtxNames("ctx->gpr[", "]");
    private static readonly string[] CtxFprScalarNames = CreateCtxNames("ctx->fpr[", "].d");

    private static string[] CreateCtxNames(string prefix, string suffix)
    {
        var names = new string[32];
        for (var index = 0; index < names.Length; index++)
        {
            names[index] = prefix + index.ToString(CultureInfo.InvariantCulture) + suffix;
        }

        return names;
    }

    private static string CtxGprName(int index) =>
        (uint)index < (uint)CtxGprNames.Length
            ? CtxGprNames[index]
            : "ctx->gpr[" + index.ToString(CultureInfo.InvariantCulture) + "]";

    private static string CtxFprScalarName(int index) =>
        (uint)index < (uint)CtxFprScalarNames.Length
            ? CtxFprScalarNames[index]
            : "ctx->fpr[" + index.ToString(CultureInfo.InvariantCulture) + "].d";

    private static string FprScalarLocalName(int index) =>
        (uint)index < (uint)FprScalarLocalNames.Length
            ? FprScalarLocalNames[index]
            : FprRegisterName(index) + ".d";

    private static string[] CreateRegisterNames(string prefix)
    {
        var names = new string[32];
        for (var index = 0; index < names.Length; index++)
        {
            names[index] = prefix + index.ToString(CultureInfo.InvariantCulture);
        }

        return names;
    }

    private static string GprRegisterName(int index) =>
        (uint)index < (uint)GprRegisterNames.Length
            ? GprRegisterNames[index]
            : "r" + index.ToString(CultureInfo.InvariantCulture);

    private static string FprRegisterName(int index) =>
        (uint)index < (uint)FprRegisterNames.Length
            ? FprRegisterNames[index]
            : "f" + index.ToString(CultureInfo.InvariantCulture);

    private static bool IsAsciiDigit(char value) => (uint)(value - '0') <= 9u;

    /// <summary>
    /// Base register name as a span over the original string. The SSA renamer
    /// appends <c>_&lt;version&gt;</c>; anything else (including an underscore
    /// followed by a non-numeric suffix) is already a base name.
    /// </summary>
    private static ReadOnlySpan<char> RegisterBaseSpan(string name)
    {
        var underscoreIdx = name.IndexOf('_');
        if (underscoreIdx < 0) return name.AsSpan();

        var suffixStart = underscoreIdx + 1;
        if (suffixStart >= name.Length) return name.AsSpan();

        for (var i = suffixStart; i < name.Length; i++)
        {
            if (!char.IsDigit(name[i])) return name.AsSpan();
        }

        return name.AsSpan(0, underscoreIdx);
    }

    /// <summary>
    /// Memo for the substring produced when a name really carries an SSA
    /// version suffix. The name universe of one function is small, so this
    /// turns a per-call allocation into a dictionary probe.
    /// </summary>
    [ThreadStatic]
    private static Dictionary<string, string>? _registerBaseNameMemo;

    private static string GetRegisterBaseName(string name)
    {
        var baseSpan = RegisterBaseSpan(name);
        if (baseSpan.Length == name.Length) return name;

        var memo = _registerBaseNameMemo ??= new Dictionary<string, string>(StringComparer.Ordinal);
        if (memo.TryGetValue(name, out var cached)) return cached;

        // The emitter runs one function at a time per thread; the bound only
        // exists so a very long translation cannot grow this without limit.
        if (memo.Count >= 8192) memo.Clear();
        var baseName = new string(baseSpan);
        memo[name] = baseName;
        return baseName;
    }

    private static bool SpanEqualsIgnoreCase(ReadOnlySpan<char> value, string candidate) =>
        value.Equals(candidate.AsSpan(), StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// Classifies a base register name and decodes its index for the indexed classes. GPR/FPR require
    /// a lowercase prefix; CR fields and special registers match case insensitively.
    /// </summary>
    private static GuestRegisterClass ClassifyRegisterBase(ReadOnlySpan<char> baseName, out int index)
    {
        index = -1;
        var length = baseName.Length;
        if (length is < 2 or > 4) return GuestRegisterClass.None;

        var first = baseName[0];
        if (length <= 3 && (first == 'r' || first == 'f') && IsAsciiDigit(baseName[1]))
        {
            if (length == 2)
            {
                index = baseName[1] - '0';
                return first == 'r' ? GuestRegisterClass.Gpr : GuestRegisterClass.Fpr;
            }

            if (IsAsciiDigit(baseName[2]))
            {
                var value = ((baseName[1] - '0') * 10) + (baseName[2] - '0');
                if (value >= 32) return GuestRegisterClass.None;
                index = value;
                return first == 'r' ? GuestRegisterClass.Gpr : GuestRegisterClass.Fpr;
            }
        }

        if (length == 3 &&
            (first is 'c' or 'C') && (baseName[1] is 'r' or 'R') &&
            IsAsciiDigit(baseName[2]) && (baseName[2] - '0') <= 7)
        {
            index = baseName[2] - '0';
            return GuestRegisterClass.CrField;
        }

        switch (length)
        {
            case 2:
                if (SpanEqualsIgnoreCase(baseName, "lr") ||
                    SpanEqualsIgnoreCase(baseName, "cr"))
                {
                    return GuestRegisterClass.Special;
                }

                break;

            case 3:
                if (SpanEqualsIgnoreCase(baseName, "ctr") ||
                    SpanEqualsIgnoreCase(baseName, "xer") ||
                    SpanEqualsIgnoreCase(baseName, "msr"))
                {
                    return GuestRegisterClass.Special;
                }

                break;

            case 4:
                if (SpanEqualsIgnoreCase(baseName, "srr0") ||
                    SpanEqualsIgnoreCase(baseName, "srr1") ||
                    SpanEqualsIgnoreCase(baseName, "hid0") ||
                    SpanEqualsIgnoreCase(baseName, "hid1") ||
                    SpanEqualsIgnoreCase(baseName, "hid2"))
                {
                    return GuestRegisterClass.Special;
                }

                if (SpanEqualsIgnoreCase(baseName[..3], "gqr") &&
                    IsAsciiDigit(baseName[3]) && (baseName[3] - '0') < 8)
                {
                    index = baseName[3] - '0';
                    return GuestRegisterClass.Special;
                }

                break;
        }

        return GuestRegisterClass.None;
    }

    private static GuestRegisterClass ClassifyRegister(string name, out int index) =>
        ClassifyRegisterBase(RegisterBaseSpan(name), out index);

    private static bool IsFloatRegister(string name) =>
        ClassifyRegister(name, out _) == GuestRegisterClass.Fpr;

    private static bool IsAbiFloatArgumentRegister(string name) =>
        ClassifyRegister(name, out var index) == GuestRegisterClass.Fpr &&
        index is >= 1 and <= 13;

    private static bool IsFloatValue(IrValue value, RepresentationEnvironment types)
    {
        if (value.Kind != "register" || string.IsNullOrWhiteSpace(value.RegisterName))
        {
            return false;
        }

        if (IsFloatRegister(value.RegisterName))
        {
            return true;
        }

        var type = types.Get(value.RegisterName);
        return type is ValueRepresentation primitive && primitive.IsFloat;
    }

    private static bool TryParseGprRegisterIndex(string registerName, out int index)
    {
        if (!string.IsNullOrWhiteSpace(registerName) &&
            registerName.StartsWith("r", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(RegisterBaseSpan(registerName)[1..], out index) &&
            index is >= 0 and <= 31)
        {
            return true;
        }

        index = -1;
        return false;
    }

    private static bool TryParseFloatRegisterIndex(string registerName, out int index)
    {
        if (!string.IsNullOrWhiteSpace(registerName) &&
            registerName.StartsWith("f", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(RegisterBaseSpan(registerName)[1..], out index) &&
            index is >= 0 and <= 31)
        {
            return true;
        }

        index = -1;
        return false;
    }

    private static bool IsCpuRegister(string name) =>
        ClassifyRegister(name, out _) != GuestRegisterClass.None;

    private static bool IsCpuRegisterNoLocalNeeded(string name) =>
        ClassifyRegister(name, out _) is GuestRegisterClass.Gpr or GuestRegisterClass.Fpr or GuestRegisterClass.Special;
}
