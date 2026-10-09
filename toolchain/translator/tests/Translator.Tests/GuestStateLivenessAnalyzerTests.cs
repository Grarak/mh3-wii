using Translator.Core.Analysis;
using Translator.Core.Ir;
using Xunit;

namespace Translator.Tests;

public sealed class GuestStateLivenessAnalyzerTests
{
    [Fact]
    public void IndirectControlFlowRequiresMaterializedContext()
    {
        var indirectCall = Function(
            new IrIndirectCall("lr", IrValue.Register("ctr"), Array.Empty<IrValue>()),
            new IrReturn(null));
        var indirectJump = Function(new IrIndirectJump(IrValue.Register("ctr")));
        var jumpTable = Function(new IrJumpTable("r3", new[]
        {
            new IrJumpTableCase(0x80001000u, "case_0")
        }));

        Assert.False(GuestStateLivenessAnalyzer.CanDeconstructWithoutContext(indirectCall));
        Assert.False(GuestStateLivenessAnalyzer.CanDeconstructWithoutContext(indirectJump));
        Assert.False(GuestStateLivenessAnalyzer.CanDeconstructWithoutContext(jumpTable));
    }

    [Fact]
    public void DirectCallPassesOnlyCalleeInputsAndReturnsOnlyCallerLiveWrites()
    {
        const uint target = 0x80002000u;
        var callee = Contract(
            gprRead: (1u << 3) | (1u << 7),
            gprWrite: (1u << 3) | (1u << 4) | (1u << 8));
        var function = Function(
            new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
            new IrBinary("r9", IrValue.Register("r4"), IrValue.Imm(1), "add"),
            new IrReturn(null));

        var result = GuestStateLivenessAnalyzer.Analyze(
            function,
            new Dictionary<uint, GuestAbiContract> { [target] = callee },
            GuestStateMask.Empty);

        var call = Assert.Single(result.DirectCalls);
        Assert.Equal((1u << 3) | (1u << 7), call.Inputs.Gpr);
        Assert.Equal(1u << 4, call.Outputs.Gpr);
        Assert.Equal(0u, call.Outputs.Gpr & ((1u << 3) | (1u << 8)));
    }

    [Fact]
    public void CalleeWriteKillsTheIncomingVersion()
    {
        const uint target = 0x80002000u;
        var callee = Contract(gprRead: 0, gprWrite: 1u << 6) with
        {
            GprDefiniteWriteMask = 1u << 6
        };
        var function = Function(
            new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
            new IrAssign("r8", IrValue.Register("r6")),
            new IrReturn(null));

        var result = GuestStateLivenessAnalyzer.Analyze(
            function,
            new Dictionary<uint, GuestAbiContract> { [target] = callee },
            GuestStateMask.Empty);

        Assert.Equal(0u, result.BlockLiveIn["entry"].Gpr & (1u << 6));
        Assert.Equal(1u << 6, Assert.Single(result.DirectCalls).Outputs.Gpr);
    }

    [Fact]
    public void ConditionalCalleeWriteDoesNotKillTheIncomingVersion()
    {
        const uint target = 0x80002000u;
        var callee = Contract(gprRead: 0, gprWrite: 1u << 6);
        var function = Function(
            new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
            new IrAssign("r8", IrValue.Register("r6")),
            new IrReturn(null));

        var result = GuestStateLivenessAnalyzer.Analyze(
            function,
            new Dictionary<uint, GuestAbiContract> { [target] = callee },
            GuestStateMask.Empty);

        Assert.Equal(1u << 6, result.BlockLiveIn["entry"].Gpr & (1u << 6));
        Assert.Equal(1u << 6, Assert.Single(result.DirectCalls).Outputs.Gpr);
    }

    [Fact]
    public void StateFreeConditionalOutputAlsoRequiresTheIncomingGpr()
    {
        var contract = Contract(gprRead: 1u << 3, gprWrite: (1u << 4) | (1u << 6)) with
        {
            GprDefiniteWriteMask = 1u << 4
        };

        var inputs = GuestStateLivenessAnalyzer.RequiredStateFreeGprInputs(
            contract, (1u << 4) | (1u << 6));

        Assert.Equal((1u << 3) | (1u << 6), inputs);
    }

    [Fact]
    public void DirectCallOrdinalsRemainStableWhenUnrelatedInstructionsDiffer()
    {
        const uint target = 0x80002000u;
        var function = Function(
            new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
            new IrAssign("r9", IrValue.Imm(1)),
            new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
            new IrReturn(null));

        var calls = GuestStateLivenessAnalyzer.Analyze(
            function,
            new Dictionary<uint, GuestAbiContract> { [target] = Contract(1u << 3, 0) },
            GuestStateMask.Empty).DirectCalls;

        Assert.Equal(new[] { 0, 1 }, calls.Select(static call => call.CallOrdinal));
        Assert.Equal(new[] { 0, 2 }, calls.Select(static call => call.InstructionIndex));
    }

    [Fact]
    public void TailCallWithoutLinkDestinationKeepsIncomingLrLive()
    {
        const uint target = 0x80002000u;
        var function = Function(
            new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
            new IrReturn(null));

        var result = GuestStateLivenessAnalyzer.Analyze(
            function,
            new Dictionary<uint, GuestAbiContract> { [target] = Contract(1u << 3, 0) },
            GuestStateMask.Empty);

        Assert.True(result.BlockLiveIn["entry"].Lr);
        Assert.True(Assert.Single(result.DirectCalls).Inputs.Lr);
    }

    [Fact]
    public void ImplicitSequentialBlockFallthroughPropagatesLaterInputsToEntry()
    {
        var function = new IrFunction("fallthrough", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrLoad("r3", new IrAddress("r3", 0), 4),
                new IrTracePpc(0x80001000u, "lwz r3, 0(r3)", "0x80630000")
            }),
            new IrBasicBlock("next", new IrInstruction[]
            {
                new IrLoad("f0", new IrAddress("r2", -4), 4),
                new IrReturn(null)
            })
        });

        var result = GuestStateLivenessAnalyzer.Analyze(
            function, new Dictionary<uint, GuestAbiContract>(), GuestStateMask.Empty);

        Assert.Equal((1u << 2) | (1u << 3), result.BlockLiveIn["entry"].Gpr);
        Assert.Equal(1u << 2, result.BlockLiveOut["entry"].Gpr);
    }

    [Fact]
    public void TracksPairedFprCrXerFpscrAndGqrState()
    {
        const uint target = 0x80002000u;
        var callee = Contract(gprRead: 0, gprWrite: 0) with
        {
            FprReadBeforeWriteMask = 1u << 2,
            FprPossibleWriteMask = (1u << 1) | (1u << 2),
            CrReadBeforeWriteMask = 1 << 3,
            CrPossibleWriteMask = (1 << 2) | (1 << 3),
            ReadsXerBeforeWrite = true,
            MayWriteXer = true,
            ReadsFpscrBeforeWrite = true,
            MayWriteFpscr = true,
            GqrReadBeforeWriteMask = 1 << 5,
            GqrPossibleWriteMask = (1 << 4) | (1 << 5)
        };
        var function = Function(
            new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
            new IrAssign("f6", IrValue.Register("f1")),
            new IrAssign("r3", IrValue.Register("cr2")),
            new IrAssign("r4", IrValue.Register("fpscr")),
            new IrAssign("r5", IrValue.Register("gqr4")),
            new IrReturn(null));

        var call = Assert.Single(GuestStateLivenessAnalyzer.Analyze(
            function,
            new Dictionary<uint, GuestAbiContract> { [target] = callee },
            GuestStateMask.Empty).DirectCalls);

        Assert.Equal(1u << 2, call.Inputs.Fpr);
        Assert.Equal(1u << 1, call.Outputs.Fpr);
        Assert.Equal(1 << 3, call.Inputs.Cr);
        Assert.Equal(1 << 2, call.Outputs.Cr);
        Assert.True(call.Inputs.Xer);
        Assert.False(call.Outputs.Xer);
        Assert.True(call.Inputs.Fpscr);
        Assert.True(call.Outputs.Fpscr);
        Assert.Equal(1 << 5, call.Inputs.Gqr);
        Assert.Equal(1 << 4, call.Outputs.Gqr);
    }

    [Fact]
    public void MaterializedContextExitKeepsNonReturnArchitecturalWritesLive()
    {
        const uint target = 0x80002000u;
        var callee = Contract(gprRead: 0, gprWrite: 0) with
        {
            FprPossibleWriteMask = 0x1Fu,
            FprReturnMask = 1u << 1,
            CrPossibleWriteMask = 1,
            MayWriteLr = true
        };
        var callerContract = Contract(gprRead: 0, gprWrite: 0) with
        {
            FprPossibleWriteMask = 0x1Fu,
            FprReturnMask = 1u << 1,
            CrPossibleWriteMask = 1,
            MayWriteLr = true
        };
        var function = Function(
            new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
            new IrReturn(null));

        var call = Assert.Single(GuestStateLivenessAnalyzer.Analyze(
            function,
            new Dictionary<uint, GuestAbiContract> { [target] = callee },
            GuestStateLivenessAnalyzer.MaterializedContextExit(callerContract)).DirectCalls);

        Assert.Equal(0x1Fu, call.Outputs.Fpr);
        Assert.Equal(1, call.Outputs.Cr);
        Assert.True(call.Outputs.Lr);
    }

    private static IrFunction Function(params IrInstruction[] instructions) =>
        new("caller", "entry", new[] { new IrBasicBlock("entry", instructions) });

    private static GuestAbiContract Contract(uint gprRead, uint gprWrite) =>
        new(
            gprRead,
            gprWrite,
            gprWrite & ((1u << 3) | (1u << 4)),
            0,
            0,
            0,
            0,
            0,
            false,
            false,
            false,
            false,
            false,
            false,
            GuestCallBoundaryFlags.None,
            Array.Empty<uint>());
}
