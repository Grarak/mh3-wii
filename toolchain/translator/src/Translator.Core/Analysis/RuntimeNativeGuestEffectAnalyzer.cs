using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace Translator.Core.Analysis;

public sealed record RuntimeNativeGuestEffectSet(
    IReadOnlyDictionary<uint, GuestAbiContract> Contracts,
    IReadOnlySet<uint> PreciseContracts,
    IReadOnlySet<uint> ConservativeContracts);

/// <summary>
/// Builds architectural effect contracts for every literal native registration. Typed stubs
/// derive their contract from the host signature; CpuContext natives are precise only when the
/// whole body reduces to direct, constant-index member access, otherwise they get a full-context contract.
/// </summary>
public static class RuntimeNativeGuestEffectAnalyzer
{
    private static readonly Regex StubRegex = GeneratedMarkers.NativeOverrideSignaturePattern();
    private static readonly Regex RegistrationRegex = GeneratedMarkers.NativeFunctionRegistrationPattern();
    private static readonly Regex FatalRegex = GeneratedMarkers.FatalStubPattern();
    private static readonly Regex FullContextRegex = new(
        @"^\s*MH3_NATIVE_FULL_CONTEXT\s*\(\s*0x(?<address>[0-9A-Fa-f]{8})\s*\)",
        RegexOptions.CultureInvariant | RegexOptions.Multiline);

    // The EABI boundary of GuestAbiContractAnalyzer.IndirectCallContract,
    // plus LR as an input (a native may look at its return address).
    private static readonly GuestAbiContract NativeEabiContract =
        GuestAbiContractAnalyzer.IndirectCallContract with { ReadsLrBeforeWrite = true };

    public static RuntimeNativeGuestEffectSet AnalyzeDirectory(string directory)
        => AnalyzeSources(NativeSourceParsing.ReadDirectory(directory));

    internal static RuntimeNativeGuestEffectSet AnalyzeSources(
        IReadOnlyList<NativeSourceFile> sources)
    {
        var contracts = new Dictionary<uint, GuestAbiContract>();
        var precise = new HashSet<uint>();
        var conservative = new HashSet<uint>();

        foreach (var sourceFile in sources)
        {
            var source = sourceFile.Content;
            var stubAddresses = new HashSet<uint>();
            foreach (Match match in StubRegex.Matches(source))
            {
                var address = ParseAddress(match.Groups["address"].Value);
                stubAddresses.Add(address);
                var symbol = match.Groups["symbol"].Value;
                var tail = match.Groups["tail"].Value;
                var isVoid = match.Groups["void"].Success;
                if (TryParseStubTail(tail, isVoid, out var returnType, out var arguments) &&
                    TryAnalyzeRegisteredFunction(source, symbol, returnType, arguments, out var contract))
                {
                    Add(address, contract, !contract.HasFullSynchronizationFence);
                }
                else
                {
                    Add(address, CompleteContextContract(), false);
                }
            }

            foreach (Match match in RegistrationRegex.Matches(source))
            {
                var address = ParseAddress(match.Groups["address"].Value);
                if (stubAddresses.Contains(address))
                    continue;
                var symbol = match.Groups["symbol"].Value;
                if (TryExtractFunction(source, symbol, out var definition) &&
                    TryAnalyzeRegisteredFunction(source, symbol, definition.ReturnRepresentation, definition.Arguments, out var contract))
                {
                    Add(address, contract, !contract.HasFullSynchronizationFence);
                }
                else
                {
                    Add(address, CompleteContextContract(), false);
                }
            }

            foreach (Match match in FatalRegex.Matches(source))
                Add(ParseAddress(match.Groups["address"].Value), CompleteContextContract(), false);
        }

        // A native whose body the analysis cannot bound still behaves as the
        // compiled function it replaces: it may run guest code (which follows
        // the PowerPC EABI) and restores or leaves r1, r2, r13-r31, f14-f31
        // and CR2-4 as they were. So it gets the EABI boundary instead of a
        // complete-context fence, unless it can suspend or switch threads or
        // is marked MH3_NATIVE_FULL_CONTEXT(0x...) (it reads or changes the
        // caller's non-volatile registers, e.g. a context switch). It stays
        // off the precise list, so its callers reach it through the dispatch.
        var fullContext = new HashSet<uint>();
        foreach (var sourceFile in sources)
            foreach (Match match in FullContextRegex.Matches(sourceFile.Content))
                fullContext.Add(ParseAddress(match.Groups["address"].Value));
        foreach (var address in contracts.Keys.ToArray())
        {
            var contract = contracts[address];
            if (!contract.HasFullSynchronizationFence || fullContext.Contains(address) ||
                (contract.BoundaryFlags & (GuestCallBoundaryFlags.CanSuspend | GuestCallBoundaryFlags.CanSwitchThreads)) != 0)
                continue;
            contracts[address] = NativeEabiContract;
        }

        // r14-r31 a native never names are preserved. A native that suspends or switches
        // threads preserves them all: each guest thread runs on its own host stack, which
        // keeps its callers' locals, and the switch saves and restores CpuContext whole.
        const uint nonvolatile = NonvolatileGprPreservationAnalyzer.NonvolatileGprMask;
        foreach (var address in contracts.Keys.ToArray())
        {
            if (fullContext.Contains(address)) continue;
            var contract = contracts[address];
            var preserved = contract.HasFullSynchronizationFence
                ? nonvolatile
                : nonvolatile & ~(contract.GprReadBeforeWriteMask | contract.GprPossibleWriteMask);
            contracts[address] = contract with
            {
                GprReadBeforeWriteMask = contract.GprReadBeforeWriteMask & ~preserved,
                GprPossibleWriteMask = contract.GprPossibleWriteMask & ~preserved,
                GprDefiniteWriteMask = contract.GprDefiniteWriteMask & ~preserved,
                GprPreservedMask = preserved
            };
        }

        return new RuntimeNativeGuestEffectSet(contracts, precise, conservative);

        void Add(uint address, GuestAbiContract contract, bool isPrecise)
        {
            // Multiple registrations for one address are safe only at the
            // intersection of their precision. A conservative contender wins.
            if (contracts.TryGetValue(address, out var previous))
            {
                contract = MergeAlternativeContracts(previous, contract);
                isPrecise = precise.Contains(address) && isPrecise && !contract.HasFullSynchronizationFence;
            }
            contracts[address] = contract;
            if (isPrecise)
            {
                precise.Add(address);
                conservative.Remove(address);
            }
            else
            {
                precise.Remove(address);
                conservative.Add(address);
            }
        }
    }

    private sealed record FunctionDefinition(string ReturnRepresentation, string Arguments, string Body, string ContextName);

    private static bool TryAnalyzeRegisteredFunction(
        string source,
        string symbol,
        string returnType,
        string arguments,
        out GuestAbiContract contract)
    {
        var argumentList = NativeSourceParsing.SplitArguments(arguments).ToArray();
        var contextArgument = argumentList
            .Select(argument => Regex.Match(argument, @"\bCpuContext\s*\*\s*(?<name>[A-Za-z_][A-Za-z0-9_]*)"))
            .FirstOrDefault(static match => match.Success);
        if (contextArgument is not null && contextArgument.Success)
        {
            if (!TryExtractFunction(source, symbol, out var definition))
            {
                contract = CompleteContextContract();
                return false;
            }
            contract = AnalyzeContextBody(definition.Body, contextArgument.Groups["name"].Value);
            return true;
        }

        if (!TryExtractFunction(source, symbol, out var typedDefinition))
        {
            contract = CompleteContextContract();
            return false;
        }
        var typedBoundaryFlags = AnalyzeBoundaryFlags(typedDefinition.Body);
        if ((typedBoundaryFlags & GuestCallBoundaryFlags.RequiresCompleteContext) != 0)
        {
            contract = CompleteContextContract(typedBoundaryFlags);
            return true;
        }

        uint gprRead = 0;
        uint fprRead = 0;
        var gpr = 3;
        var fpr = 1;
        foreach (var argument in argumentList)
        {
            var trimmed = argument.Trim();
            if (trimmed.Length == 0 || trimmed.Equals("void", StringComparison.OrdinalIgnoreCase))
                continue;
            if (NativeSourceParsing.IsFloatingPointValueArgument(trimmed))
            {
                if (fpr <= 13) fprRead |= 1u << fpr;
                ++fpr;
            }
            else
            {
                if (gpr <= 10) gprRead |= 1u << gpr;
                else gprRead |= 1u << 1; // stack argument area
                ++gpr;
            }
        }

        var normalizedReturn = returnType.Trim();
        var returnsVoid = Regex.IsMatch(normalizedReturn, @"\bvoid\s*$", RegexOptions.CultureInvariant);
        var returnsFloat = !returnsVoid && Regex.IsMatch(normalizedReturn, @"\b(float|double)\s*$", RegexOptions.CultureInvariant);
        var gprWrite = !returnsVoid && !returnsFloat ? 1u << 3 : 0u;
        var fprWrite = returnsFloat ? 1u << 1 : 0u;
        contract = new GuestAbiContract(
            gprRead, gprWrite, gprWrite,
            fprRead, fprWrite, fprWrite,
            0, 0, false, false, false, false, false, false,
            typedBoundaryFlags, Array.Empty<uint>());
        return true;
    }

    private static GuestAbiContract AnalyzeContextBody(string body, string contextName)
    {
        uint gprRead = 0, gprWrite = 0, fprRead = 0, fprWrite = 0;
        byte crRead = 0, crWrite = 0;
        var xerRead = false; var xerWrite = false;
        var ctrRead = false; var ctrWrite = false;
        var lrRead = false; var lrWrite = false;
        var flags = GuestCallBoundaryFlags.None;
        var escapedName = Regex.Escape(contextName);

        AnalyzeIndexed("gpr", 32, (index, read, write) =>
        {
            if (read) gprRead |= 1u << index;
            if (write) gprWrite |= 1u << index;
        });
        AnalyzeIndexed("fpr", 32, (index, read, write) =>
        {
            if (read) fprRead |= 1u << index;
            if (write) fprWrite |= 1u << index;
        });
        AnalyzeScalar("cr", (read, write) => { if (read) crRead = byte.MaxValue; if (write) crWrite = byte.MaxValue; });
        AnalyzeScalar("xer", (read, write) => { xerRead |= read; xerWrite |= write; });
        AnalyzeScalar("ctr", (read, write) => { ctrRead |= read; ctrWrite |= write; });
        AnalyzeScalar("lr", (read, write) => { lrRead |= read; lrWrite |= write; });

        // A computed subscript (`gpr[i]`, and also `gpr[6 + i]`, which starts
        // with a digit) names registers the scan cannot list.
        if (Regex.IsMatch(body, $@"\b{escapedName}\s*->\s*(gpr|fpr)\s*\[(?!\s*[0-9]+\s*\])", RegexOptions.CultureInvariant) ||
            Regex.IsMatch(body, $@"\b{escapedName}\s*->\s*(?!gpr\b|fpr\b|cr\b|xer\b|ctr\b|lr\b)", RegexOptions.CultureInvariant))
            flags |= GuestCallBoundaryFlags.RequiresCompleteContext;

        // Passing or aliasing the context hides arbitrary architectural access.
        // Copies (`saved = *ctx`, `*ctx = saved`) and brace initialization (`Helper h{ctx}`) too.
        if (Regex.IsMatch(body, $@"(?:\(|,)\s*{escapedName}\s*(?:,|\))", RegexOptions.CultureInvariant) ||
            Regex.IsMatch(body, $@"[=&]\s*{escapedName}\s*;", RegexOptions.CultureInvariant) ||
            Regex.IsMatch(body, $@"\*\s*{escapedName}\b(?!\s*->)", RegexOptions.CultureInvariant) ||
            Regex.IsMatch(body, $@"\{{\s*{escapedName}\s*[,}}]", RegexOptions.CultureInvariant))
            flags |= GuestCallBoundaryFlags.RequiresCompleteContext;

        flags |= AnalyzeBoundaryFlags(body);

        if ((flags & GuestCallBoundaryFlags.RequiresCompleteContext) != 0)
            return CompleteContextContract(flags);

        return new GuestAbiContract(
            gprRead, gprWrite, gprWrite & ((1u << 3) | (1u << 4)),
            fprRead, fprWrite, fprWrite & (1u << 1),
            crRead, crWrite, xerRead, xerWrite, ctrRead, ctrWrite, lrRead, lrWrite,
            flags, Array.Empty<uint>());

        void AnalyzeIndexed(string member, int count, Action<int, bool, bool> add)
        {
            var regex = new Regex($@"\b{escapedName}\s*->\s*{member}\s*\[\s*(?<index>[0-9]+)\s*\](?:\s*\.\s*[A-Za-z_][A-Za-z0-9_]*)?", RegexOptions.CultureInvariant);
            foreach (Match match in regex.Matches(body))
            {
                if (!int.TryParse(match.Groups["index"].Value, out var index) || index < 0 || index >= count)
                {
                    flags |= GuestCallBoundaryFlags.RequiresCompleteContext;
                    continue;
                }
                var (read, write) = ClassifyAccess(body, match.Index + match.Length);
                add(index, read, write);
            }
        }

        void AnalyzeScalar(string member, Action<bool, bool> add)
        {
            var regex = new Regex($@"\b{escapedName}\s*->\s*{member}\b", RegexOptions.CultureInvariant);
            foreach (Match match in regex.Matches(body))
            {
                var (read, write) = ClassifyAccess(body, match.Index + match.Length);
                add(read, write);
            }
        }
    }

    private static GuestCallBoundaryFlags AnalyzeBoundaryFlags(string body)
    {
        var flags = GuestCallBoundaryFlags.None;
        // A typed stub's contract comes from its C++ signature alone, so a body that reaches for
        // CpuContext can touch architectural state the signature never mentions. Real case:
        // 0x801AAD7C (__OSGetSystemTime) is `uint32_t (...)` but writes r3 and r4 via
        // CurrentCpuContext(); a narrowed boundary left r4 stale and wedged RFL's loader.
        if (Regex.IsMatch(body,
                @"\b(CurrentCpuContext|TryGetCpuContext|GetPersistentCpuContext|CpuContextScope)\b",
                RegexOptions.CultureInvariant))
            flags |= GuestCallBoundaryFlags.RequiresCompleteContext;
        if (Regex.IsMatch(body, @"\b(InvokeDirectCpu|InvokeIndirectCpu|InvokeIndirectJump|InvokeGuestCallback)\b", RegexOptions.CultureInvariant))
            flags |= GuestCallBoundaryFlags.InvokesGuestCode | GuestCallBoundaryFlags.RequiresCompleteContext;
        if (Regex.IsMatch(body, @"\b(YieldToScheduler|SuspendCurrent|SleepCurrent|WaitFor|BlockCurrent)\b", RegexOptions.CultureInvariant))
            flags |= GuestCallBoundaryFlags.CanSuspend | GuestCallBoundaryFlags.RequiresCompleteContext;
        if (Regex.IsMatch(body, @"\b(SwitchTo|SelectThread|RunReadyThreads|ScheduleThread)\b", RegexOptions.CultureInvariant))
            flags |= GuestCallBoundaryFlags.CanSwitchThreads | GuestCallBoundaryFlags.RequiresCompleteContext;
        return flags;
    }

    private static (bool Read, bool Write) ClassifyAccess(string body, int end)
    {
        var tail = body.AsSpan(end).TrimStart();
        if (tail.StartsWith("++", StringComparison.Ordinal) || tail.StartsWith("--", StringComparison.Ordinal)) return (true, true);
        if (tail.StartsWith("+=", StringComparison.Ordinal) || tail.StartsWith("-=", StringComparison.Ordinal) ||
            tail.StartsWith("*=", StringComparison.Ordinal) || tail.StartsWith("/=", StringComparison.Ordinal) ||
            tail.StartsWith("|=", StringComparison.Ordinal) || tail.StartsWith("&=", StringComparison.Ordinal) ||
            tail.StartsWith("^=", StringComparison.Ordinal)) return (true, true);
        if (tail.StartsWith("=", StringComparison.Ordinal) && !tail.StartsWith("==", StringComparison.Ordinal)) return (false, true);
        return (true, false);
    }

    private static bool TryParseStubTail(string tail, bool isVoid, out string returnType, out string arguments)
    {
        returnType = "void";
        arguments = string.Empty;
        var cursor = 0;
        if (!isVoid)
        {
            var comma = FindTopLevelComma(tail, cursor);
            if (comma < 0) return false;
            returnType = tail[..comma].Trim();
            cursor = comma + 1;
        }
        while (cursor < tail.Length && char.IsWhiteSpace(tail[cursor])) ++cursor;
        if (cursor >= tail.Length || tail[cursor] != '(') return false;
        var close = FindMatching(tail, cursor, '(', ')');
        if (close < 0) return false;
        arguments = tail[(cursor + 1)..close];
        return true;
    }

    private static bool TryExtractFunction(string source, string symbol, out FunctionDefinition definition)
    {
        var regex = new Regex($@"(?:extern\s+\""C\""\s+)?(?<return>[A-Za-z_][A-Za-z0-9_:<>\s*&]*?)\b{Regex.Escape(symbol)}\s*\((?<args>[^)]*)\)\s*(?:noexcept\s*)?\{{", RegexOptions.CultureInvariant);
        foreach (Match match in regex.Matches(source))
        {
            var open = match.Index + match.Length - 1;
            var close = FindMatching(source, open, '{', '}');
            if (close < 0) continue;
            var args = match.Groups["args"].Value;
            var context = Regex.Match(args, @"\bCpuContext\s*\*\s*(?<name>[A-Za-z_][A-Za-z0-9_]*)");
            definition = new FunctionDefinition(
                match.Groups["return"].Value.Trim(),
                args,
                source[(open + 1)..close],
                context.Success ? context.Groups["name"].Value : string.Empty);
            return true;
        }
        definition = null!;
        return false;
    }

    private static GuestAbiContract CompleteContextContract(GuestCallBoundaryFlags extra = GuestCallBoundaryFlags.None) =>
        new(uint.MaxValue, uint.MaxValue, (1u << 3) | (1u << 4),
            uint.MaxValue, uint.MaxValue, 1u << 1,
            byte.MaxValue, byte.MaxValue, true, true, true, true, true, true,
            extra | GuestCallBoundaryFlags.RequiresCompleteContext, Array.Empty<uint>());

    private static GuestAbiContract MergeAlternativeContracts(GuestAbiContract left, GuestAbiContract right) =>
        new(left.GprReadBeforeWriteMask | right.GprReadBeforeWriteMask,
            left.GprPossibleWriteMask | right.GprPossibleWriteMask,
            left.GprReturnMask | right.GprReturnMask,
            left.FprReadBeforeWriteMask | right.FprReadBeforeWriteMask,
            left.FprPossibleWriteMask | right.FprPossibleWriteMask,
            left.FprReturnMask | right.FprReturnMask,
            (byte)(left.CrReadBeforeWriteMask | right.CrReadBeforeWriteMask),
            (byte)(left.CrPossibleWriteMask | right.CrPossibleWriteMask),
            left.ReadsXerBeforeWrite || right.ReadsXerBeforeWrite,
            left.MayWriteXer || right.MayWriteXer,
            left.ReadsCtrBeforeWrite || right.ReadsCtrBeforeWrite,
            left.MayWriteCtr || right.MayWriteCtr,
            left.ReadsLrBeforeWrite || right.ReadsLrBeforeWrite,
            left.MayWriteLr || right.MayWriteLr,
            left.BoundaryFlags | right.BoundaryFlags,
            Array.Empty<uint>());

    private static int FindTopLevelComma(string text, int start)
    {
        var depth = 0;
        for (var index = start; index < text.Length; ++index)
        {
            if (text[index] is '(' or '[' or '<') ++depth;
            else if (text[index] is ')' or ']' or '>') depth = Math.Max(0, depth - 1);
            else if (text[index] == ',' && depth == 0) return index;
        }
        return -1;
    }

    private static int FindMatching(string text, int open, char openChar, char closeChar)
    {
        var depth = 0;
        for (var index = open; index < text.Length; ++index)
        {
            if (text[index] == openChar) ++depth;
            else if (text[index] == closeChar && --depth == 0) return index;
        }
        return -1;
    }

    private static uint ParseAddress(string value) => GuestTargetParser.ParseHexAddress(value);

}
