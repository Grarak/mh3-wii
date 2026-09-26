using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using Translator.Core.Analysis;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// Tries to emit inline C++ code for a PPC helper call instead of generating a function call.
    /// Returns true if inline code was emitted, false if the caller should fall back to function call.
    /// </summary>
    private static bool TryEmitInlinePpc(
        IrCall call,
        StringBuilder sb,
        string pad,
        RepresentationEnvironment types,
        Dictionary<string, bool> localPaired,
        StackAddressFacts stackFacts)
    {
        var target = call.Target;
        if (string.IsNullOrWhiteSpace(target))
            return false;

        // Get destination if any
        var hasDest = !string.IsNullOrWhiteSpace(call.Destination);
        var dest = hasDest ? ToDestination(call.Destination!, types) : null;

        void EmitPairedResult(string expression)
        {
            if (!hasDest) return;
            sb.AppendLine($"{pad}{PairedAssignment(call.Destination!, expression, types)}");
        }

        // Helper to get argument expression
        string GetArg(int index)
        {
            if (index >= call.Arguments.Count)
                return "0";
            return ToExpression(call.Arguments[index], types);
        }

        bool TryGetConstArg(int index, out long value)
        {
            value = 0;
            if (index >= call.Arguments.Count)
                return false;
            var argVal = call.Arguments[index];
            if (argVal.Kind != "const" || !argVal.Constant.HasValue)
                return false;
            value = argVal.Constant.Value;
            return true;
        }

        // Wrap scalar in PsFromScalar if needed for paired consumer
        string WrapForPaired(int index, bool fpscrNiDisabled = false)
        {
            if (index >= call.Arguments.Count)
                return "0";
            return ToPairedFloatExpression(
                call.Arguments[index], types, localPaired, fpscrNiDisabled);
        }

        // Extract ps0 for scalar consumers if the source is paired
        string WrapForScalar(int index)
        {
            if (index >= call.Arguments.Count)
                return "0";
            return ToScalarFloatExpression(call.Arguments[index], types, localPaired);
        }

        var guardedKnown = target.StartsWith("PPC_PsqLKnownGuarded_", StringComparison.OrdinalIgnoreCase) ||
                           target.StartsWith("PPC_PsqStKnownGuarded_", StringComparison.OrdinalIgnoreCase);
        var knownPrefix = target.StartsWith("PPC_PsqLKnownGuarded_", StringComparison.OrdinalIgnoreCase)
            ? "PPC_PsqLKnownGuarded_"
            : target.StartsWith("PPC_PsqStKnownGuarded_", StringComparison.OrdinalIgnoreCase)
                ? "PPC_PsqStKnownGuarded_"
                : target.StartsWith("PPC_PsqLKnown_", StringComparison.OrdinalIgnoreCase)
                    ? "PPC_PsqLKnown_"
                    : target.StartsWith("PPC_PsqStKnown_", StringComparison.OrdinalIgnoreCase)
                        ? "PPC_PsqStKnown_"
                        : null;
        if (knownPrefix != null &&
            uint.TryParse(target.AsSpan(knownPrefix.Length), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var knownGqr))
        {
            var knownLoad = knownPrefix.StartsWith("PPC_PsqL", StringComparison.OrdinalIgnoreCase);
            var wIndex = knownLoad ? 1 : 2;
            var iIndex = knownLoad ? 2 : 3;
            if (TryGetConstArg(wIndex, out var knownW) && TryGetConstArg(iIndex, out var knownI) &&
                knownW is 0 or 1 && knownI is >= 0 and < 8)
            {
                var address = GetArg(0);
                var stack = stackFacts.TryResolve(call.Arguments[0], out _);
                // The guard's else-arm is the generic helper, so it takes the
                // hoisted GQR local exactly like an unguarded generic site.
                var hoistedFallback = !stack && IsGqrHoisted(knownI);
                if (knownLoad && hasDest)
                {
                    var helper = stack
                        ? "PPC_PsqLKnownStackInline"
                        : "PPC_PsqLKnownInline";
                    var knownCall = $"{helper}<{knownW}u, {knownI}u, 0x{knownGqr:X8}u>(ctx, {address})";
                    var knownExpression = knownCall;
                    if (guardedKnown)
                    {
                        var fallback = hoistedFallback
                            ? $"PPC_PsqLGqrInline<{knownW}u, {knownI}u>(ctx, {HoistedGqrName(knownI)}, {address})"
                            : $"{(stack ? "PPC_PsqLStackInline" : "PPC_PsqLInline")}<{knownW}u, {knownI}u>(ctx, {address})";
                        knownExpression = $"{GqrEntryGuardName((uint)knownI, knownGqr)} ? {knownCall} : {fallback}";
                    }
                    EmitPairedResult(knownExpression);
                    return true;
                }
                if (!knownLoad)
                {
                    var helper = stack
                        ? "PPC_PsqStKnownStackInline"
                        : "PPC_PsqStKnownInline";
                    var value = WrapForPaired(1);
                    if (guardedKnown)
                    {
                        var fallback = hoistedFallback
                            ? $"PPC_PsqStGqrInline<{knownW}u, {knownI}u>(ctx, {HoistedGqrName(knownI)}, {address}, {value})"
                            : $"{(stack ? "PPC_PsqStStackInline" : "PPC_PsqStInline")}<{knownW}u, {knownI}u>(ctx, {address}, {value})";
                        sb.AppendLine($"{pad}if ({GqrEntryGuardName((uint)knownI, knownGqr)}) {helper}<{knownW}u, {knownI}u, 0x{knownGqr:X8}u>(ctx, {address}, {value});");
                        sb.AppendLine($"{pad}else {fallback};");
                    }
                    else sb.AppendLine($"{pad}{helper}<{knownW}u, {knownI}u, 0x{knownGqr:X8}u>(ctx, {address}, {value});");
                    return true;
                }
            }
        }

        if (InlinePairedCallSpecs.TryGetValue(target, out var pairedSpec))
        {
            if (hasDest)
            {
                var arguments = new string[pairedSpec.ArgumentOrder.Length];
                for (var i = 0; i < arguments.Length; i++)
                {
                    arguments[i] = WrapForPaired(
                        pairedSpec.ArgumentOrder[i], pairedSpec.NoNiVariant);
                }

                EmitPairedResult($"{pairedSpec.Helper}({string.Join(", ", arguments)})");
            }

            // The hand-written arithmetic cases historically returned true
            // without a destination, suppressing a malformed helper call. Keep
            // that behavior in the table-driven path.
            return true;
        }

        if (InlineScalarCallSpecs.TryGetValue(target, out var scalarSpec))
        {
            if (hasDest)
            {
                var arguments = new string[scalarSpec.ArgumentOrder.Length];
                for (var i = 0; i < arguments.Length; i++)
                {
                    arguments[i] = WrapForScalar(scalarSpec.ArgumentOrder[i]);
                }

                var expression = scalarSpec.Kind switch
                {
                    ScalarInlineKind.HelperCall =>
                        $"{scalarSpec.Helper}({string.Join(", ", arguments)})",
                    ScalarInlineKind.RoundedBinary =>
                        $"static_cast<double>({(scalarSpec.NoNiVariant
                            ? $"static_cast<float>({arguments[0]} {scalarSpec.Operator} {arguments[1]})"
                            : $"PpcForceSingleValueInline({arguments[0]} {scalarSpec.Operator} {arguments[1]})")})",
                    _ => throw new InvalidOperationException(
                        $"Unknown scalar inline kind '{scalarSpec.Kind}'.")
                };
                sb.AppendLine($"{pad}{dest} = {expression};");
            }

            // Match the old scalar helper cases: a missing destination is a
            // handled call that emits no statement.
            return true;
        }

        // Match and inline specific PPC helpers
        switch (InlineHelperSwitchKey(target))
        {
            // fcmpo/fcmpu lower to IrSetCrField today, but the PPC_Fcmp helper
            // writes a CR field through the thread-local CpuContext, which a
            // resident CR local can never observe. Keep a resident expansion as
            // a safety net in case a lifter path ever produces the helper form.
            case "PPC_FCMP":
                if (_activeResidency is null || !TryGetConstArg(0, out var fcmpField))
                    return false;
                sb.AppendLine($"{pad}SetCRFloatResident({_activeResidency.Cr(written: true)}, {fcmpField & 7}, {WrapForScalar(1)}, {WrapForScalar(2)});");
                return true;

            case "PPC_CRSETBIT":
                if (!hasDest)
                    return false;
                sb.AppendLine($"{pad}{dest} = PpcCrSetBitResident({dest}, static_cast<uint32_t>({GetArg(0)}), static_cast<uint32_t>({GetArg(1)}));");
                return true;

            case "PPC_CRLOGICAL":
                if (!hasDest)
                    return false;
                sb.AppendLine($"{pad}{dest} = PpcCrLogicalResident({dest}, static_cast<uint32_t>({GetArg(0)}), static_cast<uint32_t>({GetArg(1)}), static_cast<uint32_t>({GetArg(2)}), static_cast<uint32_t>({GetArg(3)}));");
                return true;

            case "PPC_MCRF":
                if (!hasDest)
                    return false;
                sb.AppendLine($"{pad}{dest} = PpcMcrfResident({dest}, static_cast<uint32_t>({GetArg(0)}), static_cast<uint32_t>({GetArg(1)}));");
                return true;

            case "PPC_CNTLZW":
                if (!hasDest)
                    return false;
                sb.AppendLine($"{pad}{dest} = PPC_CntlzwInline(static_cast<uint32_t>({GetArg(0)}));");
                return true;

            case "PPC_GETCARRY":
                if (!hasDest)
                    return false;
                {
                    var carrySource = _activeResidency is null
                        ? "ctx->xer"
                        : _activeResidency.Xer(written: false);
                    sb.AppendLine($"{pad}{dest} = ({carrySource} >> 29) & 1u;");
                }
                return true;

            case "PPC_UPDATECARRYSUB":
                if (!hasDest)
                    return false;
                sb.AppendLine($"{pad}{dest} = ({dest} & 0xDFFFFFFFu) | " +
                              $"((static_cast<uint32_t>({GetArg(0)}) >= static_cast<uint32_t>({GetArg(1)}) ? 1u : 0u) << 29);");
                return true;

            case "PPC_UPDATECARRYADD":
                if (!hasDest)
                    return false;
                sb.AppendLine($"{pad}{{");
                sb.AppendLine($"{pad}    const uint64_t ppcCarryWide = static_cast<uint64_t>(static_cast<uint32_t>({GetArg(0)})) + " +
                              $"static_cast<uint64_t>(static_cast<uint32_t>({GetArg(1)})) + " +
                              $"(static_cast<uint64_t>(static_cast<uint32_t>({GetArg(2)})) & 1u);");
                sb.AppendLine($"{pad}    {dest} = ({dest} & 0xDFFFFFFFu) | " +
                              "(static_cast<uint32_t>((ppcCarryWide >> 32) & 1u) << 29);");
                sb.AppendLine($"{pad}}}");
                return true;

            case "PPC_UPDATECARRYSHIFTRIGHT":
                if (!hasDest)
                    return false;
                sb.AppendLine($"{pad}{{");
                sb.AppendLine($"{pad}    const uint32_t ppcCarryValue = static_cast<uint32_t>({GetArg(0)});");
                sb.AppendLine($"{pad}    const uint32_t ppcCarryShift = static_cast<uint32_t>({GetArg(1)}) & 0x3Fu;");
                sb.AppendLine($"{pad}    const bool ppcCarryNegative = (ppcCarryValue & 0x80000000u) != 0;");
                sb.AppendLine($"{pad}    const uint32_t ppcCarry = ppcCarryShift == 0 ? 0u : " +
                              "(ppcCarryShift >= 32 ? (ppcCarryNegative && ppcCarryValue != 0 ? 1u : 0u) : " +
                              "(ppcCarryNegative && (ppcCarryValue & ((1u << ppcCarryShift) - 1u)) != 0 ? 1u : 0u));");
                sb.AppendLine($"{pad}    {dest} = ({dest} & 0xDFFFFFFFu) | (ppcCarry << 29);");
                sb.AppendLine($"{pad}}}");
                return true;

            case "PPC_PSFROMSCALAR":
                // PPC_PsFromScalar(value): Pack scalar float into both ps0 and ps1
                if (hasDest)
                {
                    var arg = WrapForScalar(0);
                    EmitPairedResult($"PPC_PsFromScalarInline({arg})");
                }
                return true;

            case "PPC_PSTOSCALAR":
                // PPC_PsToScalar(value): Extract ps0 as double
                if (hasDest)
                {
                    var arg = WrapForScalar(0);
                    sb.AppendLine($"{pad}{dest} = {arg};");
                }
                return true;

            case "PPC_PSMR":
                // ps_mr owns both lanes; do not lower it through the scalar
                // IrAssign path used by fmr.
                if (hasDest)
                    EmitPairedResult(GetArg(0));
                return true;

            case "PPC_PSCMPO0":
            case "PPC_PSCMPU0":
            case "PPC_PSCMPO1":
            case "PPC_PSCMPU1":
                {
                    // ps_cmpX lifts with an empty destination and the CR field as argument 0, so the
                    // resident CR local is the real destination. Falling through to the PPC_PsCmpX helper
                    // on a missing destination wrote CR through CpuContext instead, invisible to the
                    // resident local, which froze ps_cmp-gated loops and flipped branch decisions.
                    var lane = target.EndsWith("1", StringComparison.OrdinalIgnoreCase) ? 1 : 0;
                    var laneHelper = lane == 0 ? "PpcGetPs0Inline" : "PpcGetPs1Inline";
                    var left = WrapForPaired(1);
                    var right = WrapForPaired(2);
                    var crDestination = hasDest
                        ? dest
                        : _activeResidency?.Cr(written: true);
                    if (crDestination is null)
                        return false;
                    sb.AppendLine($"{pad}SetCRFloatResident({crDestination}, static_cast<uint32_t>({GetArg(0)}) & 7u, " +
                                  $"{laneHelper}({left}), {laneHelper}({right}));");
                    return true;
                }

            case "PPC_PSQL":
                if (hasDest &&
                    TryGetConstArg(1, out var loadW) &&
                    TryGetConstArg(2, out var loadI) &&
                    loadW is 0 or 1 &&
                    loadI is >= 0 and < 8)
                {
                    var addr = GetArg(0);
                    var stackLoad = stackFacts.TryResolve(call.Arguments[0], out _);
                    EmitPairedResult(!stackLoad && IsGqrHoisted(loadI)
                        ? $"PPC_PsqLGqrInline<{loadW}u, {loadI}u>(ctx, {HoistedGqrName(loadI)}, {addr})"
                        : $"{(stackLoad ? "PPC_PsqLStackInline" : "PPC_PsqLInline")}<{loadW}u, {loadI}u>(ctx, {addr})");
                    return true;
                }
                return false;

            case "PPC_PSQST":
                if (TryGetConstArg(2, out var storeW) &&
                    TryGetConstArg(3, out var storeI) &&
                    storeW is 0 or 1 &&
                    storeI is >= 0 and < 8)
                {
                    var addr = GetArg(0);
                    var value = WrapForPaired(1);
                    var stackStore = stackFacts.TryResolve(call.Arguments[0], out _);
                    sb.AppendLine(!stackStore && IsGqrHoisted(storeI)
                        ? $"{pad}PPC_PsqStGqrInline<{storeW}u, {storeI}u>(ctx, {HoistedGqrName(storeI)}, {addr}, {value});"
                        : $"{pad}{(stackStore ? "PPC_PsqStStackInline" : "PPC_PsqStInline")}<{storeW}u, {storeI}u>(ctx, {addr}, {value});");
                    return true;
                }
                return false;

            default:
                // Not handled - fall back to function call
                return false;
        }
    }

    private enum ScalarInlineKind
    {
        HelperCall,
        RoundedBinary,
    }

    private readonly record struct PairedInlineSpec(
        string Helper,
        bool NoNiVariant,
        int[] ArgumentOrder);

    private readonly record struct ScalarInlineSpec(
        ScalarInlineKind Kind,
        string? Helper,
        string? Operator,
        bool NoNiVariant,
        int[] ArgumentOrder);

    /// <summary>
    /// Descriptor table for paired arithmetic helpers. NoNI aliases are
    /// explicit entries so the helper name and scalar-to-paired conversion mode
    /// cannot drift independently.
    /// </summary>
    private static readonly IReadOnlyDictionary<string, PairedInlineSpec> InlinePairedCallSpecs =
        BuildPairedInlineCallSpecs();

    private static IReadOnlyDictionary<string, PairedInlineSpec> BuildPairedInlineCallSpecs()
    {
        var specs = new Dictionary<string, PairedInlineSpec>(StringComparer.OrdinalIgnoreCase);

        static void Add(
            Dictionary<string, PairedInlineSpec> destination,
            string target,
            string helper,
            bool noNiVariant,
            params int[] argumentOrder) =>
            destination.Add(
                target,
                new PairedInlineSpec(helper, noNiVariant, argumentOrder));

        static void AddNoNi(
            Dictionary<string, PairedInlineSpec> destination,
            string target,
            string noNiTarget,
            string helper,
            string noNiHelper,
            params int[] argumentOrder)
        {
            Add(destination, target, helper, false, argumentOrder);
            Add(destination, noNiTarget, noNiHelper, true, argumentOrder);
        }

        Add(specs, "PPC_PSMERGE00", "PPC_PsMerge00Inline", false, 0, 1);
        Add(specs, "PPC_PSMERGE01", "PPC_PsMerge01Inline", false, 0, 1);
        Add(specs, "PPC_PSMERGE10", "PPC_PsMerge10Inline", false, 0, 1);
        Add(specs, "PPC_PSMERGE11", "PPC_PsMerge11Inline", false, 0, 1);

        AddNoNi(
            specs, "PPC_PSADD", "PPC_PSADDNONI",
            "PPC_PsAddInline", "PPC_PsAddNoNiInline", 0, 1);
        AddNoNi(
            specs, "PPC_PSSUB", "PPC_PSSUBNONI",
            "PPC_PsSubInline", "PPC_PsSubNoNiInline", 0, 1);
        Add(specs, "PPC_PSDIV", "PPC_PsDivInline", false, 0, 1);
        AddNoNi(
            specs, "PPC_PSMUL", "PPC_PSMULNONI",
            "PPC_PsMulInline", "PPC_PsMulNoNiInline", 0, 1);

        Add(specs, "PPC_PSNEG", "PPC_PsNegInline", false, 0);
        Add(specs, "PPC_PSABS", "PPC_PsAbsInline", false, 0);
        Add(specs, "PPC_PSMULS0", "PPC_PsMuls0Inline", false, 0, 1);
        Add(specs, "PPC_PSMULS1", "PPC_PsMuls1Inline", false, 0, 1);

        AddNoNi(
            specs, "PPC_PSMADD", "PPC_PSMADDNONI",
            "PPC_PsMaddInline", "PPC_PsMaddNoNiInline", 0, 1, 2);
        AddNoNi(
            specs, "PPC_PSMSUB", "PPC_PSMSUBNONI",
            "PPC_PsMsubInline", "PPC_PsMsubNoNiInline", 0, 1, 2);
        AddNoNi(
            specs, "PPC_PSNMSUB", "PPC_PSNMSUBNONI",
            "PPC_PsNmsubInline", "PPC_PsNmsubNoNiInline", 0, 1, 2);
        Add(specs, "PPC_PSNMADD", "PPC_PsNmaddInline", false, 0, 1, 2);
        Add(specs, "PPC_PSMADDS0", "PPC_PsMadds0Inline", false, 0, 1, 2);
        Add(specs, "PPC_PSMADDS1", "PPC_PsMadds1Inline", false, 0, 1, 2);
        Add(specs, "PPC_PSSUM0", "PPC_PsSum0Inline", false, 0, 1, 2);
        Add(specs, "PPC_PSSUM1", "PPC_PsSum1Inline", false, 0, 1, 2);

        return specs;
    }

    /// <summary>
    /// Descriptor table for scalar arithmetic helpers. Rounded binary operators
    /// retain their distinct NI and NoNI expression forms; helper-call entries
    /// retain the runtime primitive names directly.
    /// </summary>
    private static readonly IReadOnlyDictionary<string, ScalarInlineSpec> InlineScalarCallSpecs =
        BuildScalarInlineCallSpecs();

    private static IReadOnlyDictionary<string, ScalarInlineSpec> BuildScalarInlineCallSpecs()
    {
        var specs = new Dictionary<string, ScalarInlineSpec>(StringComparer.OrdinalIgnoreCase);

        static void AddHelper(
            Dictionary<string, ScalarInlineSpec> destination,
            string target,
            string helper,
            params int[] argumentOrder) =>
            destination.Add(
                target,
                new ScalarInlineSpec(
                    ScalarInlineKind.HelperCall,
                    helper,
                    Operator: null,
                    NoNiVariant: false,
                    ArgumentOrder: argumentOrder));

        static void AddHelperNoNi(
            Dictionary<string, ScalarInlineSpec> destination,
            string target,
            string noNiTarget,
            string helper,
            string noNiHelper,
            params int[] argumentOrder)
        {
            AddHelper(destination, target, helper, argumentOrder);
            destination.Add(
                noNiTarget,
                new ScalarInlineSpec(
                    ScalarInlineKind.HelperCall,
                    noNiHelper,
                    Operator: null,
                    NoNiVariant: true,
                    ArgumentOrder: argumentOrder));
        }

        static void AddRoundedBinary(
            Dictionary<string, ScalarInlineSpec> destination,
            string target,
            string noNiTarget,
            string @operator)
        {
            destination.Add(
                target,
                new ScalarInlineSpec(
                    ScalarInlineKind.RoundedBinary,
                    Helper: null,
                    Operator: @operator,
                    NoNiVariant: false,
                    ArgumentOrder: new[] { 0, 1 }));
            destination.Add(
                noNiTarget,
                new ScalarInlineSpec(
                    ScalarInlineKind.RoundedBinary,
                    Helper: null,
                    Operator: @operator,
                    NoNiVariant: true,
                    ArgumentOrder: new[] { 0, 1 }));
        }

        AddRoundedBinary(specs, "PPC_FADDS", "PPC_FADDSNONI", "+");
        AddRoundedBinary(specs, "PPC_FSUBS", "PPC_FSUBSNONI", "-");
        AddHelperNoNi(
            specs, "PPC_FMULS", "PPC_FMULSNONI",
            "PpcFmulsInline", "PpcFmulsNoNiInline", 0, 1);
        AddRoundedBinary(specs, "PPC_FDIVS", "PPC_FDIVSNONI", "/");

        AddHelper(specs, "PPC_FSQRT", "std::sqrt", 0);
        AddHelper(specs, "PPC_FMADD", "PpcFmaddInline", 0, 1, 2);
        AddHelper(specs, "PPC_FMSUB", "PpcFmsubInline", 0, 1, 2);
        AddHelper(specs, "PPC_FNMADD", "PpcFnmaddInline", 0, 1, 2);
        AddHelper(specs, "PPC_FNMSUB", "PpcFnmsubInline", 0, 1, 2);

        return specs;
    }

    /// <summary>
    /// Case-insensitive lookup table for the remaining hand-written inline
    /// cases. Table-driven arithmetic targets are looked up before this switch.
    /// </summary>
    private static readonly HashSet<string> InlineExpandedHelperTargets = new(StringComparer.OrdinalIgnoreCase)
    {
        "PPC_FCMP", "PPC_CRSETBIT", "PPC_CRLOGICAL", "PPC_MCRF", "PPC_CNTLZW", "PPC_GETCARRY",
        "PPC_UPDATECARRYSUB", "PPC_UPDATECARRYADD", "PPC_UPDATECARRYSHIFTRIGHT", "PPC_PSFROMSCALAR",
        "PPC_PSTOSCALAR", "PPC_PSMR", "PPC_PSCMPO0", "PPC_PSCMPU0", "PPC_PSCMPO1", "PPC_PSCMPU1",
        "PPC_PSQL", "PPC_PSQST"
    };

    private static string InlineHelperSwitchKey(string target) =>
        InlineExpandedHelperTargets.TryGetValue(target, out var canonical) ? canonical : string.Empty;
}
