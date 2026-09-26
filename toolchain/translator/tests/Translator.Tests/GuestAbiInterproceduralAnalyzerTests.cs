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
