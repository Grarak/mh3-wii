using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Translator.Core.Analysis;
using Translator.Core.Build;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// Host-register calling convention for a translated-to-translated direct call. The first
    /// argument GPRs (r3-r10) and FPRs (f1-f8) the callee reads before writing travel as C
    /// arguments, and r3/r4 (or f1) come back as the C return value, so neither side stores
    /// them to <c>CpuContext</c>. Everything else still goes through the context exactly as the
    /// contract describes. Derived only from the published contract, so every caller and the
    /// callee agree; the masks are part of the symbol, so a disagreement fails to link.
    /// </summary>
    internal sealed record RegArgsSignature(uint Address, int[] ArgGprs, int[] ArgFprs, int[] RetGprs, int RetFpr)
    {
        public uint ArgGprMask => Mask(ArgGprs);
        public uint ArgFprMask => Mask(ArgFprs);
        public uint RetGprMask => Mask(RetGprs);
        public uint RetFprMask => RetFpr < 0 ? 0u : 1u << RetFpr;

        public string Symbol => $"func_{Address:X8}_ra{ArgGprMask:x}_{ArgFprMask:x}_{RetGprMask:x}_{RetFprMask:x}";

        public string ReturnType => RetGprs.Length switch
        {
            1 => "uint32_t",
            2 => "uint64_t",
            _ => RetFpr >= 0 ? "double" : "void"
        };

        public bool HasResult => RetGprs.Length != 0 || RetFpr >= 0;

        public string Parameters(bool names)
        {
            var parameters = new List<string> { names ? CpuContextDefinitionParameter : "CpuContext*" };
            parameters.AddRange(ArgGprs.Select(gpr => names ? $"uint32_t arg_r{gpr}" : "uint32_t"));
            parameters.AddRange(ArgFprs.Select(fpr => names ? $"double arg_f{fpr}" : "double"));
            return string.Join(", ", parameters);
        }

        public string Declaration => $"extern \"C\" {ReturnType} {Symbol}({Parameters(names: false)});";

        public string Definition => $"extern \"C\" {ReturnType} {Symbol}({Parameters(names: true)})";

        /// <summary>The C return expression built from per-register value expressions.</summary>
        public string Pack(Func<int, string> gpr, Func<int, string> fpr) => RetGprs.Length switch
        {
            1 => gpr(RetGprs[0]),
            2 => $"(static_cast<uint64_t>({gpr(RetGprs[1])}) << 32) | {gpr(RetGprs[0])}",
            _ => fpr(RetFpr)
        };

        /// <summary>Assignments that spread a returned value <paramref name="result"/> into registers.</summary>
        public IEnumerable<string> Unpack(string result, Func<int, string> gpr, Func<int, string> fpr)
        {
            if (RetGprs.Length == 1)
            {
                yield return $"{gpr(RetGprs[0])} = {result};";
            }
            else if (RetGprs.Length == 2)
            {
                yield return $"{gpr(RetGprs[0])} = static_cast<uint32_t>({result});";
                yield return $"{gpr(RetGprs[1])} = static_cast<uint32_t>({result} >> 32);";
            }
            else if (RetFpr >= 0)
            {
                yield return $"{fpr(RetFpr)} = {result};";
            }
        }

        private static uint Mask(IEnumerable<int> registers) =>
            registers.Aggregate(0u, static (mask, index) => mask | (1u << index));
    }

    // AAPCS hands the first four integer arguments over in r0-r3 and ctx takes one of them.
    private const int RegArgsMaxGprArguments = 3;
    private const int RegArgsMaxFprArguments = 8;

    /// <summary>
    /// The register-argument signature of a translated function, or null when a direct call to it
    /// has to stay on the plain <c>CpuContext</c> entry: a complete-context fence, an RSO overlay
    /// (its calls check module execution), a diagnostics target the shard emitter keeps generic,
    /// or nothing to pass at all.
    /// </summary>
    internal static RegArgsSignature? TryGetRegArgsSignature(uint address, GuestAbiContract? contract)
    {
        if (contract is null ||
            contract.HasFullSynchronizationFence ||
            address is >= 0x80800000u and < 0x81800000u ||
            TranslatedBuildShardEmitter.RequiresGenericDirectCall(address))
        {
            return null;
        }

        var argGprs = Enumerable.Range(3, 8)
            .Where(gpr => (contract.GprReadBeforeWriteMask & (1u << gpr)) != 0)
            .Take(RegArgsMaxGprArguments)
            .ToArray();
        var argFprs = Enumerable.Range(1, 8)
            .Where(fpr => (contract.FprReadBeforeWriteMask & (1u << fpr)) != 0)
            .Take(RegArgsMaxFprArguments)
            .ToArray();
        // A result register must hold a defined value on every exit: either the callee always
        // writes it, or it arrived as an argument and still holds the caller's value otherwise.
        var argGprMask = argGprs.Aggregate(0u, static (mask, gpr) => mask | (1u << gpr));
        var argFprMask = argFprs.Aggregate(0u, static (mask, fpr) => mask | (1u << fpr));
        var retGprMask = contract.GprPossibleWriteMask & (argGprMask | contract.GprDefiniteWriteMask) &
                         ((1u << 3) | (1u << 4));
        var retGprs = new[] { 3, 4 }.Where(gpr => (retGprMask & (1u << gpr)) != 0).ToArray();
        // One C return value: AAPCS returns a GPR/FPR mix in memory, so f1 only rides alone.
        var retFpr = retGprs.Length == 0 &&
                     (contract.FprPossibleWriteMask & (argFprMask | contract.FprDefiniteWriteMask) & (1u << 1)) != 0
            ? 1
            : -1;
        if (argGprs.Length == 0 && argFprs.Length == 0 && retGprs.Length == 0 && retFpr < 0)
            return null;
        return new RegArgsSignature(address, argGprs, argFprs, retGprs, retFpr);
    }

    /// <summary>
    /// Register-argument signature for a direct call site, or null when the call keeps the plain
    /// boundary: no residency to carry values, a target a mod may replace, or no signature.
    /// </summary>
    private static RegArgsSignature? ResolveRegArgsCallSignature(
        uint target,
        IReadOnlyDictionary<uint, GuestAbiContract> guestAbiContracts,
        IReadOnlySet<uint> modOverridableCallTargets)
    {
        if (_activeResidency is null ||
            modOverridableCallTargets.Contains(target) ||
            _activeRegArgsFunctions?.Contains(target) != true)
        {
            return null;
        }
        return TryGetRegArgsSignature(target, guestAbiContracts.GetValueOrDefault(target));
    }

    /// <summary>
    /// The signature of the body being emitted when it is the register-argument variant; its exits
    /// return the result registers instead of publishing them.
    /// </summary>
    [ThreadStatic]
    private static RegArgsSignature? _activeRegArgsBody;

    /// <summary>Functions the body being emitted may call with register arguments.</summary>
    [ThreadStatic]
    private static IReadOnlySet<uint>? _activeRegArgsFunctions;

    /// <summary>
    /// A return from the body being emitted. <paramref name="fromContext"/> exits (tail dispatch,
    /// LR continuation) left CpuContext authoritative; the others return the resident locals.
    /// </summary>
    private static string ReturnStatement(bool fromContext)
    {
        if (_activeRegArgsBody is not { HasResult: true } signature) return "return;";
        var residency = !fromContext ? _activeResidency : null;
        return "return " + signature.Pack(
            gpr => residency?.HasGpr(gpr) == true ? RegisterResidency.GprLocal(gpr) : $"ctx->gpr[{gpr}]",
            fpr => residency?.HasFpr(fpr) == true ? RegisterResidency.FprLocal(fpr) + ".d" : $"ctx->fpr[{fpr}].d") + ";";
    }

    /// <summary>
    /// The <c>CpuContext</c> entry of a register-argument body: unpacks, calls, publishes. The call
    /// stays a call: inlining the body here would duplicate every function (14% more text).
    /// </summary>
    private static void AppendRegArgsContextWrapper(StringBuilder sb, string name, RegArgsSignature signature)
    {
        sb.AppendLine(FunctionDefinitionSignature(name));
        sb.AppendLine("{");
        var call = $"{signature.Symbol}({string.Join(", ", RegArgsContextArguments(signature))})";
        if (signature.HasResult)
        {
            sb.AppendLine($"    {signature.ReturnType} result;");
            sb.AppendLine($"    [[clang::noinline]] result = {call};");
            foreach (var store in signature.Unpack("result", static gpr => $"ctx->gpr[{gpr}]", static fpr => $"ctx->fpr[{fpr}].d"))
                sb.AppendLine($"    {store}");
        }
        else
        {
            sb.AppendLine($"    [[clang::noinline]] {call};");
        }
        sb.AppendLine("}");
    }

    /// <summary>
    /// A register-argument entry for a body that can't be the variant itself (state-free clone,
    /// GQR versioning, no residency): it publishes the arguments and calls the ordinary entry.
    /// Callers only know the signature, so every eligible base function must define one.
    /// </summary>
    private static void AppendRegArgsAdapter(StringBuilder sb, string name, RegArgsSignature signature)
    {
        sb.AppendLine(signature.Definition);
        sb.AppendLine("{");
        foreach (var gpr in signature.ArgGprs) sb.AppendLine($"    ctx->gpr[{gpr}] = arg_r{gpr};");
        foreach (var fpr in signature.ArgFprs) sb.AppendLine($"    ctx->fpr[{fpr}].d = arg_f{fpr};");
        sb.AppendLine($"    [[clang::noinline]] {name}(ctx);");
        if (signature.HasResult)
            sb.AppendLine($"    return {signature.Pack(static gpr => $"ctx->gpr[{gpr}]", static fpr => $"ctx->fpr[{fpr}].d")};");
        sb.AppendLine("}");
    }

    private static IEnumerable<string> RegArgsContextArguments(RegArgsSignature signature) =>
        new[] { "ctx" }
            .Concat(signature.ArgGprs.Select(static gpr => $"ctx->gpr[{gpr}]"))
            .Concat(signature.ArgFprs.Select(static fpr => $"ctx->fpr[{fpr}].d"));

    /// <summary>
    /// A direct call through the register-argument variant when the target statically binds to its
    /// base translation (<c>KnownRegArgsCpuCall</c>), else the ordinary boundary. Arguments and
    /// results skip the context; the rest of the contract syncs as usual.
    /// </summary>
    private static void EmitRegArgsDirectCall(
        StringBuilder sb,
        string pad,
        int bufferBaseLength,
        uint address,
        RegArgsSignature signature,
        ResidencyBoundarySync boundarySync)
    {
        var residency = _activeResidency!;
        var inner = pad + "    ";
        sb.AppendLine($"{pad}if constexpr (KnownRegArgsCpuCall<0x{address:X8}u>(&func_{address:X8})) {{");
        var passedGprs = signature.ArgGprMask | signature.RetGprMask;
        var passedFprs = signature.ArgFprMask | signature.RetFprMask;
        AppendFlush(sb, inner, boundarySync with
        {
            FlushGprMask = boundarySync.FlushGprMask & ~passedGprs,
            FlushFprMask = boundarySync.FlushFprMask & ~passedFprs
        });
        var arguments = new[] { "ctx" }
            .Concat(signature.ArgGprs.Select(gpr => residency.Gpr(gpr, written: false)))
            .Concat(signature.ArgFprs.Select(fpr => residency.FprStorage(fpr, written: false) + ".d"));
        var call = $"{signature.Symbol}({string.Join(", ", arguments)})";
        if (signature.HasResult)
        {
            // Uniquified by the call site's position in the translation unit.
            var result = $"regargs_result_{address:X8}_{bufferBaseLength + sb.Length:X}";
            sb.AppendLine($"{inner}const auto {result} = {call};");
            foreach (var store in signature.Unpack(
                         result,
                         gpr => residency.Gpr(gpr, written: true),
                         fpr => residency.FprStorage(fpr, written: true) + ".d"))
            {
                sb.AppendLine($"{inner}{store}");
            }
        }
        else
        {
            sb.AppendLine($"{inner}{call};");
        }
        AppendReload(sb, inner, boundarySync with
        {
            ReloadGprMask = boundarySync.ReloadGprMask & ~signature.RetGprMask,
            ReloadFprMask = boundarySync.ReloadFprMask & ~signature.RetFprMask
        });
        sb.AppendLine($"{pad}}} else {{");
        AppendFlush(sb, inner, boundarySync);
        RecordEmittedDirectCallTarget(address);
        sb.AppendLine($"{inner}InvokeDirectCpu<0x{address:X8}u>(ctx);");
        AppendReload(sb, inner, boundarySync);
        sb.AppendLine($"{pad}}}");
    }

    /// <summary>
    /// Declarations every register-argument call site in this unit needs: the callee's plain entry
    /// (the binding check compares against it) and its variant.
    /// </summary>
    private static IEnumerable<string> RegArgsCallDeclarations(
        IEnumerable<uint> directCallTargets,
        IReadOnlyDictionary<uint, GuestAbiContract> guestAbiContracts,
        IReadOnlySet<uint> modOverridableCallTargets,
        IReadOnlySet<uint> regArgsFunctions)
    {
        foreach (var target in directCallTargets.Distinct().Order())
        {
            if (modOverridableCallTargets.Contains(target) ||
                !regArgsFunctions.Contains(target) ||
                TryGetRegArgsSignature(target, guestAbiContracts.GetValueOrDefault(target)) is not { } signature)
            {
                continue;
            }
            yield return $"extern \"C\" void func_{target:X8}(CpuContext* ctx);";
            yield return signature.Declaration;
        }
    }
}
