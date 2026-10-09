using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using Translator.Core.Ir;
using Translator.Core.Loading;
using Translator.Core.Translation;
using Xunit;

namespace Translator.Tests;

/// <summary>
/// IR-level leaf inlining (performance audit T-INLINE). The splice happens before SSA, so every
/// negative case is a silent-corruption bug if it starts firing; hence more rejection tests than positive ones.
/// </summary>
public class LeafInliningTests
{
    private const uint Caller = MemoryLayout.RamBase;
    private const uint Callee = MemoryLayout.RamBase + 0x100;
    private const uint Callee2 = MemoryLayout.RamBase + 0x180;
    private const int ImageSize = 0x400;

    // --- minimal PowerPC encoder -------------------------------------------

    private static uint Blr() => 0x4E800020u;
    private static uint Bl(uint from, uint to) => 0x48000001u | ((to - from) & 0x03FFFFFCu);
    private static uint B(uint from, uint to) => 0x48000000u | ((to - from) & 0x03FFFFFCu);
    private static uint Bne(uint from, uint to) => 0x40820000u | ((to - from) & 0xFFFCu);
    private static uint Addi(int d, int a, int simm) =>
        0x38000000u | ((uint)d << 21) | ((uint)a << 16) | (uint)(simm & 0xFFFF);
    private static uint Lwz(int d, int a, int offset) =>
        0x80000000u | ((uint)d << 21) | ((uint)a << 16) | (uint)(offset & 0xFFFF);
    private static uint Stwu(int s, int a, int offset) =>
        0x94000000u | ((uint)s << 21) | ((uint)a << 16) | (uint)(offset & 0xFFFF);
    private static uint Add(int d, int a, int b) =>
        0x7C000214u | ((uint)d << 21) | ((uint)a << 16) | ((uint)b << 11);
    private static uint Cmpwi(int crf, int a, int simm) =>
        0x2C000000u | ((uint)crf << 23) | ((uint)a << 16) | (uint)(simm & 0xFFFF);
    private static uint Mflr(int d) => 0x7C0802A6u | ((uint)d << 21);
    private static uint Bctrl() => 0x4E800421u;
    private static uint Bctr() => 0x4E800420u;

    private static ProgramImage BuildImage(params (uint Address, uint Word)[] words)
    {
        var memory = new byte[ImageSize];
        foreach (var (address, word) in words)
        {
            BinaryPrimitives.WriteUInt32BigEndian(
                memory.AsSpan((int)(address - MemoryLayout.RamBase), 4), word);
        }

        return new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, ImageSize),
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, ImageSize),
            default,
            "leaf-inlining");
    }

    private static TranslationOptions Options(
        bool inlining = true,
        IReadOnlySet<uint>? blocked = null,
        int maxCalleeInstructions = 32,
        int maxGrowthPercent = 50,
        bool multiBlock = true) =>
        TranslationOptions.Default with
        {
            AllowUnsupportedInstructions = true,
            KnownFunctionEntryPoints = new HashSet<uint> { Caller, Callee, Callee2 },
            EnableLeafInlining = inlining,
            LeafInliningBlockedTargets = blocked,
            LeafInliningMaxCalleeInstructions = maxCalleeInstructions,
            LeafInliningMaxCallerGrowthPercent = maxGrowthPercent,
            LeafInliningAllowMultiBlockCallees = multiBlock
        };

    private static FunctionTranslationResult Translate(
        ProgramImage image, uint entryPoint, TranslationOptions options) =>
        new FunctionTranslator(image).Translate(entryPoint, options);

    /// <summary>Caller that calls the leaf once and returns.</summary>
    private static (uint Address, uint Word)[] SimpleCaller(uint callee = Callee) =>
    [
        (Caller + 0, Addi(3, 3, 4)),
        (Caller + 4, Bl(Caller + 4, callee)),
        (Caller + 8, Addi(3, 3, 1)),
        (Caller + 12, Blr())
    ];

    // --- positive cases -----------------------------------------------------

    [Fact]
    public void CalleeWritesLandInTheCallerAndTheCallDisappears()
    {
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Lwz(3, 3, 8)),
                (Callee + 4, Blr())
            ]);

        var inlined = Translate(image, Caller, Options());
        var reference = Translate(image, Caller, Options(inlining: false));

        Assert.Equal(1, inlined.Metrics.InlinedCallSites);
        Assert.Equal(2, inlined.Metrics.InlinedGuestInstructions);
        Assert.Equal(0, reference.Metrics.InlinedCallSites);
        Assert.Contains("InvokeDirectCpu<0x80000100u>(ctx);", reference.CxxCode, StringComparison.Ordinal);
        Assert.DoesNotContain("InvokeDirectCpu<0x80000100u>(ctx);", inlined.CxxCode);
        // The callee's load is now a load of the caller's own r3.
        Assert.Contains("MemoryInline::FlatRead32", inlined.CxxCode, StringComparison.Ordinal);
        Assert.Contains("inline leaf 0x80000100", inlined.CxxCode, StringComparison.Ordinal);
    }

    [Fact]
    public void ArgumentRegistersAreReadFromTheCallerStateAtTheCallPoint()
    {
        // The caller computes r3 = r3 + 4 immediately before the call, and the
        // callee's first act is to consume r3. After the splice, the callee's
        // read must see the +4 value without any context round trip.
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Add(3, 3, 4)),
                (Callee + 4, Blr())
            ]);

        var result = Translate(image, Caller, Options());

        Assert.Equal(1, result.Metrics.InlinedCallSites);
        // Residency keeps r3 in a local across the splice, so there is no flush
        // or reload of the guest register file where the call used to be.
        Assert.DoesNotContain("InvokeDirectCpu<0x80000100u>(ctx);", result.CxxCode);
        Assert.Contains("r3 = ", result.CxxCode, StringComparison.Ordinal);
        var ir = result.LinearIr.Blocks.SelectMany(block => block.Instructions).ToArray();
        Assert.DoesNotContain(ir, instruction => instruction is IrCall call && call.Target == "0x80000100");
    }

    [Fact]
    public void ConditionRegisterWrittenInsideTheSpliceReachesTheCallersBranch()
    {
        // The callee sets CR0; the caller branches on it. Fusing the two across
        // the splice is only possible because the compare became an ordinary
        // instruction of the caller.
        var image = BuildImage(
            [
                (Caller + 0, Bl(Caller + 0, Callee)),
                (Caller + 4, Bne(Caller + 4, Caller + 12)),
                (Caller + 8, Addi(3, 3, 1)),
                (Caller + 12, Blr()),
                (Callee + 0, Cmpwi(0, 4, 0)),
                (Callee + 4, Blr())
            ]);

        var inlined = Translate(image, Caller, Options());
        var reference = Translate(image, Caller, Options(inlining: false));

        Assert.Equal(1, inlined.Metrics.InlinedCallSites);
        // Without inlining the branch has to reload the architectural CR after
        // the call because the producing compare lives in another function.
        Assert.Contains("InvokeDirectCpu<0x80000100u>(ctx);", reference.CxxCode, StringComparison.Ordinal);
        Assert.Equal(2, CountOccurrences(reference.CxxCode, "cr = ctx->cr;"));
        // With inlining the compare writes the caller's resident CR directly and
        // the branch reads it with no context round trip at all.
        Assert.Contains(
            "SetCRResident(cr, xer, 0, static_cast<int32_t>(r4), static_cast<int32_t>(0));",
            inlined.CxxCode, StringComparison.Ordinal);
        Assert.Equal(1, CountOccurrences(inlined.CxxCode, "cr = ctx->cr;"));
    }

    [Fact]
    public void CompareInsideTheSpliceIsFusedIntoTheFollowingBranch()
    {
        // Both successors immediately overwrite CR0, which is what makes the
        // spliced compare's only consumer the caller's branch.
        var image = BuildImage(
            [
                (Caller + 0, Bl(Caller + 0, Callee)),
                (Caller + 4, Bne(Caller + 4, Caller + 16)),
                (Caller + 8, Cmpwi(0, 3, 0)),
                (Caller + 12, Blr()),
                (Caller + 16, Cmpwi(0, 5, 0)),
                (Caller + 20, Blr()),
                (Callee + 0, Cmpwi(0, 4, 7)),
                (Callee + 4, Blr())
            ]);

        var code = Translate(image, Caller, Options()).CxxCode;

        Assert.Contains(
            "if ((static_cast<int32_t>(r4) != static_cast<int32_t>(7)))",
            code, StringComparison.Ordinal);
    }

    [Fact]
    public void TheLinkRegisterWriteOfAnInlinedCallIsNotEmitted()
    {
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Addi(4, 4, 1)),
                (Callee + 4, Blr())
            ]);

        var code = Translate(image, Caller, Options()).CxxCode;

        Assert.DoesNotContain("ctx->lr = 0x80000008", code);
        Assert.DoesNotContain("lr = 0x80000008u", code);
    }

    [Fact]
    public void TheStandaloneCalleeBodyIsStillTranslatedOnItsOwn()
    {
        // Indirect dispatch, mod overrides and the registry all resolve the
        // callee by address, so the splice must never be a replacement for it.
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Lwz(3, 3, 8)),
                (Callee + 4, Blr())
            ]);

        var standalone = Translate(image, Callee, Options());

        Assert.Contains("func_80000100", standalone.CxxCode, StringComparison.Ordinal);
        Assert.Equal(0, standalone.Metrics.InlinedCallSites);
    }

    [Fact]
    public void EveryCallSiteOfTheSameCalleeIsSplicedIndependently()
    {
        var image = BuildImage(
            [
                (Caller + 0, Bl(Caller + 0, Callee)),
                (Caller + 4, Bl(Caller + 4, Callee)),
                (Caller + 8, Blr()),
                (Callee + 0, Lwz(3, 3, 8)),
                (Callee + 4, Blr())
            ]);

        var result = Translate(image, Caller, Options());

        Assert.Equal(2, result.Metrics.InlinedCallSites);
        Assert.Equal(2, CountOccurrences(result.CxxCode, "inline leaf 0x80000100"));
        Assert.DoesNotContain("InvokeDirectCpu<0x80000100u>(ctx);", result.CxxCode);
    }

    [Fact]
    public void TailCallToALeafIsSplicedAheadOfTheCallersReturn()
    {
        var image = BuildImage(
            [
                (Caller + 0, Addi(3, 3, 4)),
                (Caller + 4, B(Caller + 4, Callee)),
                (Callee + 0, Lwz(3, 3, 8)),
                (Callee + 4, Blr())
            ]);

        var result = Translate(image, Caller, Options());

        Assert.Equal(1, result.Metrics.InlinedCallSites);
        Assert.DoesNotContain("InvokeDirectCpu<0x80000100u>(ctx);", result.CxxCode);
    }

    // --- acyclic multi-block callees ----------------------------------------

    /// <summary>Compare, branch, one arm, join - the shape of nw4r list walkers.</summary>
    private static (uint Address, uint Word)[] DiamondCallee() =>
    [
        (Callee + 0, Cmpwi(0, 3, 0)),
        (Callee + 4, Bne(Callee + 4, Callee + 12)),
        (Callee + 8, Addi(3, 3, 1)),
        (Callee + 12, Blr())
    ];

    [Fact]
    public void AnAcyclicMultiBlockCalleeIsSplicedWithItsOwnControlFlow()
    {
        var image = BuildImage([.. SimpleCaller(), .. DiamondCallee()]);

        var result = Translate(image, Caller, Options());

        Assert.Equal(1, result.Metrics.InlinedCallSites);
        Assert.Equal(4, result.Metrics.InlinedGuestInstructions);
        Assert.DoesNotContain("InvokeDirectCpu<0x80000100u>(ctx);", result.CxxCode);
        // The callee's blocks are now blocks of the caller, under call-site
        // local labels, and the branch skips the arm exactly as it did inside
        // the callee.
        Assert.Contains("loc_inl0_0x80000100:", result.CxxCode, StringComparison.Ordinal);
        Assert.Contains("loc_inl0_0x80000108:", result.CxxCode, StringComparison.Ordinal);
        Assert.Contains("loc_inl0_0x8000010C:", result.CxxCode, StringComparison.Ordinal);
        Assert.Contains("goto loc_inl0_0x8000010C;", result.CxxCode, StringComparison.Ordinal);
        // The callee's return became the caller's continuation, which is where
        // the code after the call site now lives.
        Assert.Contains("loc_inl0_cont_80000100:", result.CxxCode, StringComparison.Ordinal);
        Assert.Equal(
            new IrJump("inl0_cont_80000100"),
            Block(result, "inl0_0x8000010C").Instructions[^1]);
        result.Ssa.ValidateUseDef();
    }

    [Fact]
    public void TheCallersOwnCodeAroundAMultiBlockSpliceStillRuns()
    {
        // The call site sits in the middle of the caller's only block, so the
        // splice has to cut that block in two and keep both halves wired up.
        var image = BuildImage([.. SimpleCaller(), .. DiamondCallee()]);

        var result = Translate(image, Caller, Options());
        var labels = result.LinearIr.Blocks.Select(block => block.Label).ToArray();

        // Entry block keeps its label, so the function still starts where the
        // emitter's prologue jumps.
        Assert.Equal("0x80000000", labels[0]);
        Assert.Equal(result.LinearIr.EntryLabel, labels[0]);
        Assert.Equal(labels.Length, labels.Distinct(StringComparer.OrdinalIgnoreCase).Count());
        // Caller prefix, the callee's three blocks, then the continuation that
        // carries everything the caller did after the call.
        Assert.Equal(
            [
                "0x80000000",
                "inl0_0x80000100",
                "inl0_0x80000104",
                "inl0_0x80000108",
                "inl0_0x8000010C",
                "inl0_cont_80000100"
            ],
            labels);
        Assert.IsType<IrReturn>(result.LinearIr.Blocks[^1].Instructions[^1]);
    }

    [Fact]
    public void EachMultiBlockCallSiteGetsItsOwnCopyOfTheCalleesLabels()
    {
        var image = BuildImage(
            [
                (Caller + 0, Bl(Caller + 0, Callee)),
                (Caller + 4, Bl(Caller + 4, Callee)),
                (Caller + 8, Blr()),
                .. DiamondCallee()
            ]);

        var result = Translate(image, Caller, Options(maxGrowthPercent: 100_000));

        Assert.Equal(2, result.Metrics.InlinedCallSites);
        Assert.Contains("loc_inl0_cont_80000100:", result.CxxCode, StringComparison.Ordinal);
        Assert.Contains("loc_inl1_cont_80000100:", result.CxxCode, StringComparison.Ordinal);
        result.Ssa.ValidateUseDef();
    }

    [Fact]
    public void SplicedLabelsCanNeverBeReadBackAsAGuestAddress()
    {
        // A `loc_XXXXXXXX` label is how the emitter reports an interior resume
        // point of the caller. A spliced copy of the callee is not one, so its
        // labels must not parse as an address.
        var image = BuildImage([.. SimpleCaller(), .. DiamondCallee()]);

        var result = Translate(image, Caller, Options());

        var spliced = result.LinearIr.Blocks
            .Select(block => block.Label)
            .Where(label => label.StartsWith("inl", StringComparison.Ordinal))
            .ToArray();
        Assert.NotEmpty(spliced);
        foreach (var label in spliced)
        {
            Assert.False(
                uint.TryParse(label, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out _),
                $"Spliced label '{label}' is readable as a guest address.");
        }
    }

    [Fact]
    public void ARaisedInstructionCapAdmitsALeafTheOldCapRefused()
    {
        // 45 arithmetic instructions plus the terminating blr: past the old cap
        // of 32, inside the current one.
        var words = new List<(uint, uint)>(SimpleCaller());
        for (var index = 0; index < 45; index++)
        {
            words.Add((Callee + (uint)(index * 4), Addi(3, 3, 1)));
        }

        words.Add((Callee + 180, Blr()));
        var image = BuildImage([.. words]);

        Assert.Equal(56, TranslationOptions.Default.LeafInliningMaxCalleeInstructions);
        Assert.Equal(
            1,
            Translate(image, Caller, Options(maxCalleeInstructions: 56, maxGrowthPercent: 100_000))
                .Metrics.InlinedCallSites);
        Assert.Equal(
            0,
            Translate(image, Caller, Options(maxCalleeInstructions: 32, maxGrowthPercent: 100_000))
                .Metrics.InlinedCallSites);
    }

    // --- rejection cases ----------------------------------------------------

    [Fact]
    public void AMultiBlockCalleeIsRejectedWhenMultiBlockSplicingIsOff()
    {
        var image = BuildImage([.. SimpleCaller(), .. DiamondCallee()]);

        Assert.Equal(0, Translate(image, Caller, Options(multiBlock: false)).Metrics.InlinedCallSites);
        Assert.Equal(1, Translate(image, Caller, Options()).Metrics.InlinedCallSites);
    }

    [Fact]
    public void ACalleeWithALoopIsRejected()
    {
        // The back edge to the entry makes this a loop, not a one-way region
        // between the call point and its continuation.
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Addi(4, 4, -1)),
                (Callee + 4, Cmpwi(0, 4, 0)),
                (Callee + 8, Bne(Callee + 8, Callee + 0)),
                (Callee + 12, Blr())
            ]);

        AssertNotInlined(image);
    }

    [Fact]
    public void AMultiBlockCalleeThatTouchesTheLinkRegisterIsRejected()
    {
        // Reachable only on one arm, and still disqualifying: the call's own LR
        // write is dropped, so the mflr would read the caller's incoming value.
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Cmpwi(0, 3, 0)),
                (Callee + 4, Bne(Callee + 4, Callee + 12)),
                (Callee + 8, Mflr(0)),
                (Callee + 12, Blr())
            ]);

        AssertNotInlined(image);
    }

    [Fact]
    public void AMultiBlockCalleeThatCallsAnotherFunctionIsRejected()
    {
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Cmpwi(0, 3, 0)),
                (Callee + 4, Bne(Callee + 4, Callee + 12)),
                (Callee + 8, Bl(Callee + 8, Callee2)),
                (Callee + 12, Blr()),
                (Callee2 + 0, Addi(3, 3, 1)),
                (Callee2 + 4, Blr())
            ]);

        AssertNotInlined(image);
    }

    [Fact]
    public void AMultiBlockCalleeIsStillSubjectToTheBlockList()
    {
        var image = BuildImage([.. SimpleCaller(), .. DiamondCallee()]);

        // A patch site on the callee's second block is still a patch site.
        Assert.Equal(
            0,
            Translate(image, Caller, Options(blocked: new HashSet<uint> { Callee + 8 })).Metrics.InlinedCallSites);
    }

    [Fact]
    public void AdmittingMultiBlockCalleesCannotChangeACallerThatHasNone()
    {
        // The invariant the whole transform rests on: a caller whose admitted
        // set does not change must emit byte-identical text.
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Lwz(3, 3, 8)),
                (Callee + 4, Blr())
            ]);

        var withMultiBlock = Translate(image, Caller, Options());
        var withoutMultiBlock = Translate(image, Caller, Options(multiBlock: false));
        var atTheOldCap = Translate(image, Caller, Options(maxCalleeInstructions: 32));
        var atTheNewCap = Translate(image, Caller, Options(maxCalleeInstructions: 56));

        Assert.Equal(1, withMultiBlock.Metrics.InlinedCallSites);
        Assert.Equal(withoutMultiBlock.CxxCode, withMultiBlock.CxxCode);
        Assert.Equal(atTheOldCap.CxxCode, atTheNewCap.CxxCode);
    }

    [Fact]
    public void InliningIsOffUnlessRequested()
    {
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Lwz(3, 3, 8)),
                (Callee + 4, Blr())
            ]);

        var result = Translate(image, Caller, Options(inlining: false));

        Assert.Equal(0, result.Metrics.InlinedCallSites);
        Assert.Contains("InvokeDirectCpu<0x80000100u>(ctx);", result.CxxCode, StringComparison.Ordinal);
    }

    [Fact]
    public void ACalleeThatCallsAnotherFunctionIsRejected()
    {
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Bl(Callee + 0, Callee2)),
                (Callee + 4, Blr()),
                (Callee2 + 0, Addi(3, 3, 1)),
                (Callee2 + 4, Blr())
            ]);

        AssertNotInlined(image);
    }

    [Fact]
    public void ACalleeWithAnIndirectCallIsRejected()
    {
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Bctrl()),
                (Callee + 4, Blr())
            ]);

        AssertNotInlined(image);
    }

    [Fact]
    public void ACalleeWithAnIndirectBranchIsRejected()
    {
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Bctr())
            ]);

        AssertNotInlined(image);
    }

    [Fact]
    public void ACalleeThatTouchesTheLinkRegisterIsRejected()
    {
        // The call's own LR write is dropped, so a callee that observes LR
        // would read the caller's incoming value instead of a return address.
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Mflr(0)),
                (Callee + 4, Blr())
            ]);

        AssertNotInlined(image);
    }

    [Fact]
    public void ACalleeThatCreatesAStackFrameIsRejected()
    {
        // Splicing a second r1 epoch into the caller invalidates the stack
        // aliasing model used after inlining, even when the callee restores r1.
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Stwu(1, 1, -16)),
                (Callee + 4, Addi(3, 3, 1)),
                (Callee + 8, Addi(1, 1, 16)),
                (Callee + 12, Blr())
            ]);

        AssertNotInlined(image);
    }

    [Fact]
    public void AnOversizedCalleeIsRejected()
    {
        var body = new List<(uint, uint)>(SimpleCaller());
        for (var index = 0; index < 8; index++)
        {
            body.Add((Callee + (uint)(index * 4), Addi(3, 3, 1)));
        }
        body.Add((Callee + 32, Blr()));

        var image = BuildImage([.. body]);

        Assert.Equal(1, Translate(image, Caller, Options(maxCalleeInstructions: 16)).Metrics.InlinedCallSites);
        Assert.Equal(0, Translate(image, Caller, Options(maxCalleeInstructions: 4)).Metrics.InlinedCallSites);
    }

    [Fact]
    public void ABlockedAddressAnywhereInsideTheCalleeIsRejected()
    {
        // A native registration or mod patch inside the decoded body means the
        // runtime winner is not these bytes.
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Lwz(3, 3, 8)),
                (Callee + 4, Addi(3, 3, 1)),
                (Callee + 8, Blr())
            ]);

        Assert.Equal(
            0,
            Translate(image, Caller, Options(blocked: new HashSet<uint> { Callee })).Metrics.InlinedCallSites);
        Assert.Equal(
            0,
            Translate(image, Caller, Options(blocked: new HashSet<uint> { Callee + 4 })).Metrics.InlinedCallSites);
        Assert.Equal(1, Translate(image, Caller, Options()).Metrics.InlinedCallSites);
    }

    [Fact]
    public void GrowthBudgetStopsInliningOnceTheCallerWouldBalloon()
    {
        var words = new List<(uint, uint)>
        {
            (Caller + 0, Bl(Caller + 0, Callee)),
            (Caller + 4, Bl(Caller + 4, Callee)),
            (Caller + 8, Bl(Caller + 8, Callee)),
            (Caller + 12, Blr())
        };
        for (var index = 0; index < 12; index++)
        {
            words.Add((Callee + (uint)(index * 4), Addi(3, 3, 1)));
        }
        words.Add((Callee + 48, Blr()));

        var image = BuildImage([.. words]);

        var generous = Translate(image, Caller, Options(maxGrowthPercent: 100_000)).Metrics.InlinedCallSites;
        var stingy = Translate(image, Caller, Options(maxGrowthPercent: 0)).Metrics.InlinedCallSites;

        Assert.Equal(3, generous);
        Assert.InRange(stingy, 0, 2);
    }

    [Fact]
    public void ContinuationDispatchDisablesInliningForTheWholeCaller()
    {
        // Non-returning and continuation call sites place labels keyed by guest
        // instruction address inside the emitted body; a duplicated guest
        // instruction stream cannot coexist with that.
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Lwz(3, 3, 8)),
                (Callee + 4, Blr())
            ]);

        var options = Options() with
        {
            LrContinuationCallTargets = new HashSet<uint> { Callee2 }
        };

        Assert.Equal(0, Translate(image, Caller, options).Metrics.InlinedCallSites);
    }

    [Fact]
    public void ModuleTranslationsAreNeverInlined()
    {
        var image = BuildImage(
            [
                .. SimpleCaller(),
                (Callee + 0, Lwz(3, 3, 8)),
                (Callee + 4, Blr())
            ]);

        var options = Options() with
        {
            ModuleLinkBase = 0x80800000u,
            ModuleGuestBase = 0x80900000u,
            ModuleLinkedCodeSize = 0x1000
        };

        Assert.Equal(0, Translate(image, Caller, options).Metrics.InlinedCallSites);
    }

    [Fact]
    public void ASelfRecursiveCallIsNeverInlined()
    {
        var image = BuildImage(
            [
                (Caller + 0, Bl(Caller + 0, Caller)),
                (Caller + 4, Blr())
            ]);

        Assert.Equal(0, Translate(image, Caller, Options()).Metrics.InlinedCallSites);
    }

    // --- classifier unit tests ---------------------------------------------

    [Fact]
    public void CandidateClassifierReportsWhyACalleeWasRefused()
    {
        var policy = new LeafInliningPolicy();

        LeafFunctionInliner.TryCreateCandidate(
            0x80000100u,
            new IrFunction("two_blocks", "a",
            [
                new IrBasicBlock("a", [new IrJump("b")]),
                new IrBasicBlock("b", [new IrReturn(null)])
            ]),
            Array.Empty<Translator.Core.Disassembly.PpcInstruction>(),
            policy,
            out var jumpBetweenBlocks);
        Assert.Equal(LeafInlineRejection.ControlFlow, jumpBetweenBlocks);

        LeafFunctionInliner.TryCreateCandidate(
            0x80000100u,
            new IrFunction("many_blocks", "a",
            [
                new IrBasicBlock("a", []),
                new IrBasicBlock("b", []),
                new IrBasicBlock("c", []),
                new IrBasicBlock("d", []),
                new IrBasicBlock("e", [new IrReturn(null)])
            ]),
            Array.Empty<Translator.Core.Disassembly.PpcInstruction>(),
            policy,
            out var multiBlock);
        Assert.Equal(LeafInlineRejection.MultipleBlocks, multiBlock);

        LeafFunctionInliner.TryCreateCandidate(
            0x80000100u,
            Single(new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()), new IrReturn(null)),
            Array.Empty<Translator.Core.Disassembly.PpcInstruction>(),
            policy,
            out var guestCall);
        Assert.Equal(LeafInlineRejection.GuestCall, guestCall);

        LeafFunctionInliner.TryCreateCandidate(
            0x80000100u,
            Single(new IrCall(string.Empty, "OSSystemCall", Array.Empty<IrValue>()), new IrReturn(null)),
            Array.Empty<Translator.Core.Disassembly.PpcInstruction>(),
            policy,
            out var fence);
        Assert.Equal(LeafInlineRejection.OpaqueHelper, fence);

        LeafFunctionInliner.TryCreateCandidate(
            0x80000100u,
            Single(new IrAssign("r0", IrValue.Register("lr")), new IrReturn(null)),
            Array.Empty<Translator.Core.Disassembly.PpcInstruction>(),
            policy,
            out var link);
        Assert.Equal(LeafInlineRejection.TouchesLinkRegister, link);

        LeafFunctionInliner.TryCreateCandidate(
            0x80000100u,
            Single(new IrAssign("r1", IrValue.Imm(0)), new IrReturn(null)),
            Array.Empty<Translator.Core.Disassembly.PpcInstruction>(),
            policy,
            out var stackPointer);
        Assert.Equal(LeafInlineRejection.TouchesStackPointer, stackPointer);

        LeafFunctionInliner.TryCreateCandidate(
            0x80000100u,
            Single(new IrAssign("r3", IrValue.Imm(1))),
            Array.Empty<Translator.Core.Disassembly.PpcInstruction>(),
            policy,
            out var noReturn);
        Assert.Equal(LeafInlineRejection.NoTerminatingReturn, noReturn);

        var accepted = LeafFunctionInliner.TryCreateCandidate(
            0x80000100u,
            Single(
                new IrTracePpc(0x80000100u, "addi r3,r3,1", "0x38630001"),
                new IrAssign("r3", IrValue.Imm(1)),
                new IrReturn(null)),
            Array.Empty<Translator.Core.Disassembly.PpcInstruction>(),
            policy,
            out var none);
        Assert.Equal(LeafInlineRejection.None, none);
        Assert.NotNull(accepted);
        Assert.Equal(1, accepted!.GuestInstructionCount);
        // The terminating return became a fall-through.
        Assert.DoesNotContain(accepted.Body, instruction => instruction is IrReturn);
    }

    [Fact]
    public void CandidateClassifierAdmitsAnAcyclicRegionAndRefusesALoop()
    {
        var policy = new LeafInliningPolicy();

        // if (cr0) goto join; arm; join: return
        var diamond = LeafFunctionInliner.TryCreateCandidate(
            0x80000100u,
            new IrFunction("diamond", "0x80000100",
            [
                new IrBasicBlock("0x80000100",
                [
                    new IrTracePpc(0x80000100u, "cmpwi r3,0", "0x2C030000"),
                    new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                    new IrTracePpc(0x80000104u, "bne 0x8000010C", "0x40820008"),
                    new IrBranch("ne", "0x8000010C", "0x80000108")
                ]),
                new IrBasicBlock("0x80000108",
                [
                    new IrTracePpc(0x80000108u, "addi r3,r3,1", "0x38630001"),
                    new IrAssign("r3", IrValue.Imm(1))
                ]),
                new IrBasicBlock("0x8000010C",
                [
                    new IrTracePpc(0x8000010Cu, "blr", "0x4E800020"),
                    new IrReturn(null)
                ])
            ]),
            Array.Empty<Translator.Core.Disassembly.PpcInstruction>(),
            policy,
            out var acyclic);
        Assert.Equal(LeafInlineRejection.None, acyclic);
        Assert.NotNull(diamond);
        Assert.Equal(4, diamond!.GuestInstructionCount);
        Assert.Empty(diamond.Body);
        Assert.NotNull(diamond.Blocks);
        Assert.Equal(3, diamond.Blocks!.Count);
        // The arm's implicit fall-through onto the join is materialized, so the
        // splice does not depend on where the caller places these blocks.
        Assert.Equal(new IrJump("0x8000010C"), diamond.Blocks[1].Instructions[^1]);

        // Same shape, but the branch goes back to the entry.
        LeafFunctionInliner.TryCreateCandidate(
            0x80000100u,
            new IrFunction("loop", "0x80000100",
            [
                new IrBasicBlock("0x80000100",
                [
                    new IrTracePpc(0x80000100u, "cmpwi r3,0", "0x2C030000"),
                    new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                    new IrTracePpc(0x80000104u, "bne 0x80000100", "0x4082FFFC"),
                    new IrBranch("ne", "0x80000100", "0x80000108")
                ]),
                new IrBasicBlock("0x80000108",
                [
                    new IrTracePpc(0x80000108u, "blr", "0x4E800020"),
                    new IrReturn(null)
                ])
            ]),
            Array.Empty<Translator.Core.Disassembly.PpcInstruction>(),
            policy,
            out var cyclic);
        Assert.Equal(LeafInlineRejection.CyclicControlFlow, cyclic);

        // Turning the shape off restores the straight-line-only verdict.
        LeafFunctionInliner.TryCreateCandidate(
            0x80000100u,
            new IrFunction("diamond", "0x80000100",
            [
                new IrBasicBlock("0x80000100",
                [
                    new IrTracePpc(0x80000100u, "b 0x80000104", "0x48000004"),
                    new IrJump("0x80000104")
                ]),
                new IrBasicBlock("0x80000104",
                [
                    new IrTracePpc(0x80000104u, "blr", "0x4E800020"),
                    new IrReturn(null)
                ])
            ]),
            Array.Empty<Translator.Core.Disassembly.PpcInstruction>(),
            policy with { AllowAcyclicMultiBlockCallees = false },
            out var disabled);
        Assert.Equal(LeafInlineRejection.ControlFlow, disabled);
    }

    private static IrFunction Single(params IrInstruction[] instructions) =>
        new("leaf", "entry", [new IrBasicBlock("entry", instructions)]);

    private static IrBasicBlock Block(FunctionTranslationResult result, string label) =>
        result.LinearIr.Blocks.Single(
            block => string.Equals(block.Label, label, StringComparison.Ordinal));

    private static void AssertNotInlined(ProgramImage image)
    {
        var result = Translate(image, Caller, Options());
        Assert.Equal(0, result.Metrics.InlinedCallSites);
        Assert.Contains("InvokeDirectCpu<0x80000100u>(ctx);", result.CxxCode, StringComparison.Ordinal);
    }

    private static int CountOccurrences(string haystack, string needle)
    {
        var count = 0;
        var index = haystack.IndexOf(needle, StringComparison.Ordinal);
        while (index >= 0)
        {
            count++;
            index = haystack.IndexOf(needle, index + needle.Length, StringComparison.Ordinal);
        }

        return count;
    }
}
