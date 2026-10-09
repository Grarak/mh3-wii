using Translator.Core.Analysis;
using Translator.Core.Ir;

namespace Translator.Tests;

public sealed class StackAddressFactsTests
{
    [Fact]
    public void TracksFramePointerCopiesAndSubtractions()
    {
        var function = new IrFunction("stack_facts", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r11", IrValue.Register("r1"), IrValue.Imm(64), "add"),
                new IrAssign("save_area", IrValue.Register("r11")),
                new IrBinary("slot", IrValue.Register("save_area"), IrValue.Imm(8), "sub")
            })
        });

        var facts = StackAddressFacts.Build(function);

        Assert.True(facts.TryResolve(new IrAddress("r11", -28), out var frameOffset));
        Assert.Equal(36, frameOffset);
        Assert.True(facts.TryResolve(new IrAddress("slot", 0), out var slotOffset));
        Assert.Equal(56, slotOffset);
        Assert.True(facts.ContainsTemporary("r11"));
        Assert.True(facts.ContainsTemporary("save_area"));
    }

    [Fact]
    public void TreatsEveryR1VersionAsTheCurrentStackRoot()
    {
        var facts = StackAddressFacts.Build(new IrFunction("r1_versions", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("r1_7", IrValue.Register("r4"))
            })
        }));

        Assert.True(facts.TryResolve(new IrAddress("r1_7", 12), out var offset));
        Assert.Equal(12, offset);
    }

    [Fact]
    public void DoesNotAddCfgOrCommutedExpressionSemantics()
    {
        var facts = StackAddressFacts.Build(new IrFunction("simple_only", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("commuted", IrValue.Imm(8), IrValue.Register("r1"), "add"),
                new IrPhi("joined", new Dictionary<string, string>
                {
                    ["entry"] = "r1",
                    ["other"] = "r1"
                })
            })
        }));

        Assert.False(facts.ContainsTemporary("commuted"));
        Assert.False(facts.ContainsTemporary("joined"));
        Assert.False(facts.TryResolve(new IrAddress("commuted", 0), out _));
    }
}
