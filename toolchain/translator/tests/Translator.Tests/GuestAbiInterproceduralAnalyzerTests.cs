using Translator.Core.Analysis;
using Translator.Core.Ir;
using Xunit;

namespace Translator.Tests;

public sealed class GuestAbiInterproceduralAnalyzerTests
{
    [Fact]
    public void CallerDefinitionSuppressesCalleeReadAtFunctionEntry()
    {
        const uint caller = 0x80001000u;
        const uint callee = 0x80002000u;
        var functions = new Dictionary<uint, IrFunction>
        {
            [caller] = Function("caller",
                new IrAssign("r3", IrValue.Imm(7)),
                new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)),
            [callee] = Function("callee",
                new IrAssign("r4", IrValue.Register("r3")),
                new IrReturn(null))
        };

        var result = GuestAbiInterproceduralAnalyzer.Analyze(functions);

        Assert.Equal(0u, result.Contracts[caller].GprReadBeforeWriteMask & (1u << 3));
        Assert.NotEqual(0u, result.Contracts[caller].GprPossibleWriteMask & ((1u << 3) | (1u << 4)));
        Assert.Equal(1u << 3, result.Contracts[callee].GprReadBeforeWriteMask);
    }

    [Fact]
    public void IndirectCallsFollowTheEabiInsteadOfFencing()
    {
        const uint caller = 0x80001000u;
        const uint virtualCaller = 0x80002000u;
        var functions = new Dictionary<uint, IrFunction>
        {
            [caller] = Function("caller",
                new IrAssign("r30", IrValue.Imm(1)),
                new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)),
            [virtualCaller] = Function("virtualCaller",
                new IrIndirectCall("lr", IrValue.Register("ctr"), Array.Empty<IrValue>()),
                new IrIndirectJump(IrValue.Register("ctr")))
        };

        var result = GuestAbiInterproceduralAnalyzer.Analyze(functions);
        var contract = result.Contracts[virtualCaller];

        Assert.False(contract.HasFullSynchronizationFence);
        Assert.False(result.Contracts[caller].HasFullSynchronizationFence);
        // Argument and scratch registers are inputs, the volatile set is
        // clobbered; r14-r31 and f14-f31 survive the call.
        Assert.Equal(0x00003FFFu, contract.GprReadBeforeWriteMask);
        Assert.Equal(0x00001FF9u, contract.GprPossibleWriteMask);
        Assert.Equal(0x00003FFFu, contract.FprPossibleWriteMask);
        Assert.Equal(0u, contract.GprPossibleWriteMask & 0xFFFFC000u);
        Assert.NotEqual(0, (int)(contract.BoundaryFlags & GuestCallBoundaryFlags.InvokesGuestCode));
    }

    [Fact]
    public void NoReturnCalleeDoesNotSpreadItsFenceOrEffects()
    {
        const uint caller = 0x80001000u;
        const uint panic = 0x80002000u;
        var functions = new Dictionary<uint, IrFunction>
        {
            [caller] = Function("caller",
                new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
                new IrAssign("r31", IrValue.Register("r3")),
                new IrReturn(null)),
            [panic] = Function("panic",
                new IrAssign("r4", IrValue.Register("r20")),
                new IrAssign("r5", IrValue.Register("r3")),
                new IrUndefined(0x80002008u, 0x7C000008u, "trap"),
                new IrReturn(null))
        };

        var fenced = GuestAbiInterproceduralAnalyzer.Analyze(functions);
        Assert.True(fenced.Contracts[caller].HasFullSynchronizationFence);

        var result = GuestAbiInterproceduralAnalyzer.Analyze(
            functions, noReturnFunctions: new HashSet<uint> { panic });
        var callerContract = result.Contracts[caller];
        Assert.False(callerContract.HasFullSynchronizationFence);
        Assert.False(result.Contracts[panic].HasFullSynchronizationFence);
        // Only r1-r5 and r13 of what the panic reads reach the caller: r20 does not.
        Assert.Equal(0u, callerContract.GprReadBeforeWriteMask & ~0x0000203Eu);
        Assert.NotEqual(0u, callerContract.GprReadBeforeWriteMask & (1u << 3));
        Assert.Equal(1u << 31, callerContract.GprPossibleWriteMask);
        Assert.Equal(0u, result.Contracts[panic].GprPossibleWriteMask);
    }

    [Fact]
    public void PathThroughNoReturnCallDoesNotCountTowardTheContract()
    {
        const uint caller = 0x80001000u;
        const uint panic = 0x80002000u;
        var fail = new IrBasicBlock("fail", new IrInstruction[]
        {
            new IrAssign("r6", IrValue.Register("r20")),
            new IrAssign("r7", IrValue.Imm(1)),
            new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
            new IrJump("done")
        });
        var entry = new IrBasicBlock("entry", new IrInstruction[]
        {
            new IrBranch("eq", "fail", "done")
        });
        var done = new IrBasicBlock("done", new IrInstruction[]
        {
            new IrAssign("r3", IrValue.Register("r4")),
            new IrReturn(null)
        });
        var functions = new Dictionary<uint, IrFunction>
        {
            [caller] = new IrFunction("caller", "entry", new[] { entry, fail, done }),
            [panic] = Function("panic",
                new IrIndirectCall("lr", IrValue.Register("ctr"), Array.Empty<IrValue>()),
                new IrReturn(null))
        };

        var result = GuestAbiInterproceduralAnalyzer.Analyze(
            functions, noReturnFunctions: new HashSet<uint> { panic });
        var callerContract = result.Contracts[caller];

        Assert.False(callerContract.HasFullSynchronizationFence);
        Assert.Equal(0u, callerContract.GprReadBeforeWriteMask & ((1u << 20) | (1u << 6) | (1u << 7)));
        Assert.Equal(0u, callerContract.GprPossibleWriteMask & ((1u << 6) | (1u << 7)));
        Assert.Equal(1u << 3, callerContract.GprDefiniteWriteMask & (1u << 3));
    }

    [Fact]
    public void KnownDirectCallIgnoresSyntacticAbiArgumentsNotReadByCallee()
    {
        const uint caller = 0x80001000u;
        const uint callee = 0x80002000u;
        var functions = new Dictionary<uint, IrFunction>
        {
            [caller] = Function("caller",
                new IrCall("lr", "0x80002000", new[]
                {
                    IrValue.Register("r3"),
                    IrValue.Register("r10"),
                    IrValue.Register("f1"),
                    IrValue.Register("f13")
                }),
                new IrReturn(null)),
            [callee] = Function("callee",
                new IrAssign("r4", IrValue.Register("r3")),
                new IrReturn(null))
        };

        var result = GuestAbiInterproceduralAnalyzer.Analyze(functions);
        var callerContract = result.Contracts[caller];

        Assert.Equal(1u << 3, callerContract.GprReadBeforeWriteMask);
        Assert.Equal(0u, callerContract.FprReadBeforeWriteMask);
    }

    [Fact]
    public void RecursiveFunctionsFormOneComponentAndReachFixedPoint()
    {
        const uint first = 0x80001000u;
        const uint second = 0x80002000u;
        var functions = new Dictionary<uint, IrFunction>
        {
            [first] = Function("first",
                new IrAssign("r5", IrValue.Register("r3")),
                new IrCall("lr", "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)),
            [second] = Function("second",
                new IrAssign("r6", IrValue.Register("r4")),
                new IrCall("lr", "0x80001000", Array.Empty<IrValue>()),
                new IrReturn(null))
        };

        var result = GuestAbiInterproceduralAnalyzer.Analyze(functions);

        Assert.Contains(result.StronglyConnectedComponents,
            component => component.SequenceEqual(new[] { first, second }));
        Assert.Equal((1u << 3) | (1u << 4),
            result.Contracts[first].GprReadBeforeWriteMask & ((1u << 3) | (1u << 4)));
        Assert.Equal((1u << 3) | (1u << 4),
            result.Contracts[second].GprReadBeforeWriteMask & ((1u << 3) | (1u << 4)));
    }

    [Fact]
    public void DeepCallGraphDoesNotConsumeTheNativeStack()
    {
        const int count = 20_000;
        const uint start = 0x80000000u;
        var functions = new Dictionary<uint, IrFunction>(count);
        for (var index = 0; index < count; ++index)
        {
            var address = start + (uint)(index * 4);
            functions[address] = index + 1 == count
                ? Function($"f{index}", new IrReturn(null))
                : Function($"f{index}",
                    new IrCall("lr", $"0x{address + 4:X8}", Array.Empty<IrValue>()),
                    new IrReturn(null));
        }

        var result = GuestAbiInterproceduralAnalyzer.Analyze(functions);

        Assert.Equal(count, result.StronglyConnectedComponents.Count);
        Assert.All(result.StronglyConnectedComponents, static component => Assert.Single(component));
    }

    private static IrFunction Function(string name, params IrInstruction[] instructions) =>
        new(name, "entry", new[] { new IrBasicBlock("entry", instructions) });
}
