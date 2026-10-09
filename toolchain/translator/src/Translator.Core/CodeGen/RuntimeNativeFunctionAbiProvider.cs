using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace Translator.Core.CodeGen;

public sealed class RuntimeNativeFunctionAbiProvider : IGuestFunctionAbiProvider
{
    private static readonly Regex StubVoidRegex = GeneratedMarkers.NativeOverrideVoidArgumentsPattern();

    private readonly Dictionary<string, GuestFunctionAbi> _abis;

    public RuntimeNativeFunctionAbiProvider(IReadOnlyDictionary<uint, GuestFunctionAbi> abis)
    {
        _abis = abis.ToDictionary(
            static item => $"func_{item.Key:X8}",
            static item => item.Value,
            StringComparer.OrdinalIgnoreCase);
    }

    public bool TryGetGuestFunctionAbi(string target, out GuestFunctionAbi abi)
    {
        if (GuestTargetParser.TryParseAddress(target, out var address))
        {
            return _abis.TryGetValue($"func_{address:X8}", out abi!);
        }

        return _abis.TryGetValue(target, out abi!);
    }

    public static RuntimeNativeFunctionAbiProvider LoadVoidStubAbisFromDirectory(string directory)
    {
        return new RuntimeNativeFunctionAbiProvider(AnalyzeVoidStubAbisFromDirectory(directory));
    }

    /// <summary>
    /// Builds the declared-ABI provider, trusting only void stubs whose addresses are in
    /// <paramref name="includeAddresses"/> (the project's <c>native_abi_directories</c>); stub
    /// signatures outside that set are documentation, not ABI contracts.
    /// </summary>
    public static RuntimeNativeFunctionAbiProvider FromIndex(
        RuntimeNativeIndex index, IReadOnlySet<uint> includeAddresses)
    {
        var abis = index.VoidStubAbis
            .Where(entry => includeAddresses.Contains(entry.Address))
            .ToDictionary(
            static entry => entry.Address,
            static entry => new GuestFunctionAbi(
                argumentRegisters: entry.ArgumentRegisters,
                preservesVolatileContext: true,
                writesGprReturnRegister: false,
                writesFloatReturnRegister: false,
                scalarFloatArgumentRegisters: entry.ScalarFloatArgumentRegisters));
        return new RuntimeNativeFunctionAbiProvider(abis);
    }

    public static IReadOnlyDictionary<uint, GuestFunctionAbi> AnalyzeVoidStubAbisFromDirectory(string directory)
        => AnalyzeVoidStubAbis(NativeSourceParsing.ReadDirectory(directory));

    internal static IReadOnlyDictionary<uint, GuestFunctionAbi> AnalyzeVoidStubAbis(
        IReadOnlyList<NativeSourceFile> sources)
    {
        var abis = new Dictionary<uint, GuestFunctionAbi>();

        foreach (var sourceFile in sources.Where(static source =>
                     source.FullPath.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase)))
        {
            var content = sourceFile.Content;
            foreach (Match match in StubVoidRegex.Matches(content))
            {
                var args = match.Groups["args"].Value;
                if (args.Contains("CpuContext", StringComparison.Ordinal))
                {
                    continue;
                }

                if (!GuestTargetParser.TryParseHexAddress(match.Groups["addr"].Value, out var address))
                {
                    continue;
                }

                var argumentRegisters = InferArgumentRegisters(args);
                abis[address] = new GuestFunctionAbi(
                    argumentRegisters: argumentRegisters,
                    preservesVolatileContext: true,
                    writesGprReturnRegister: false,
                    writesFloatReturnRegister: false,
                    scalarFloatArgumentRegisters: argumentRegisters.Where(static r => r.StartsWith("f", StringComparison.OrdinalIgnoreCase)));
            }
        }

        return abis;
    }

    private static IReadOnlyList<string> InferArgumentRegisters(string args)
    {
        var result = new List<string>();
        var gpr = 3;
        var fpr = 1;
        foreach (var argument in NativeSourceParsing.SplitArguments(args))
        {
            var trimmed = argument.Trim();
            if (trimmed.Length == 0 || string.Equals(trimmed, "void", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            if (NativeSourceParsing.IsFloatingPointValueArgument(trimmed))
            {
                if (fpr <= 13)
                {
                    result.Add($"f{fpr}");
                }
                fpr++;
            }
            else
            {
                if (gpr <= 10)
                {
                    result.Add($"r{gpr}");
                }
                gpr++;
            }
        }

        return result;
    }

}
