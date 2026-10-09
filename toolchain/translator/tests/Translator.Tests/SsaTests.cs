using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;
using Xunit;

namespace Translator.Tests;

public class SsaTests
{
    [Fact]
    public void InsertsPhiAndRenamesUses()
    {
        var blocks = new List<IrBasicBlock>
        {
            new("entry", new List<IrInstruction>
            {
                new IrAssign("r3", IrValue.Imm(5)),
                new IrBranch("bne", "then", "else")
            }),
            new("then", new List<IrInstruction>
            {
                new IrAssign("r3", IrValue.Imm(1)),
                new IrJump("merge")
            }),
            new("else", new List<IrInstruction>
            {
                new IrAssign("r3", IrValue.Imm(2)),
                new IrJump("merge")
            }),
            new("merge", new List<IrInstruction>
            {
                new IrBinary("r4", IrValue.Register("r3"), IrValue.Imm(0), "add"),
                new IrReturn(IrValue.Register("r4"))
            })
        };

        var func = new IrFunction("test", "entry", blocks);
        var ssa = new SsaTransformer().Convert(func);
        ssa.ValidateUseDef();

        var merge = ssa.Function.Blocks.Single(b => b.Label == "merge");
        Assert.IsType<IrPhi>(merge.Instructions[0]);
        var phi = (IrPhi)merge.Instructions[0];

        Assert.Equal(2, phi.Sources.Count);
        Assert.Contains("then", phi.Sources.Keys);
        Assert.Contains("else", phi.Sources.Keys);

        // All registers should be versioned.
        Assert.All(ssa.Function.Blocks.SelectMany(b => b.Instructions), ins =>
        {
            switch (ins)
            {
                case IrAssign a:
                    Assert.Contains("_", a.Destination);
                    break;
                case IrBinary b:
                    Assert.Contains("_", b.Destination);
                    Assert.Contains("_", b.Left.RegisterName ?? string.Empty);
                    break;
            }
        });
    }

    [Fact]
    public void FloatPhiAllowsImplicitLiveIn()
    {
        var blocks = new List<IrBasicBlock>
        {
            new("entry", new List<IrInstruction>
            {
                new IrBranch("bne", "left", "right")
            }),
            new("left", new List<IrInstruction>
            {
                new IrAssign("f1", IrValue.Imm(0)),
                new IrJump("merge")
            }),
            new("right", new List<IrInstruction>
            {
                new IrJump("merge")
            }),
            new("merge", new List<IrInstruction>
            {
                new IrReturn(IrValue.Register("f1"))
            })
        };

        var func = new IrFunction("float_phi", "entry", blocks);
        var ssa = new SsaTransformer().Convert(func);
        ssa.ValidateUseDef();

        var merge = ssa.Function.Blocks.Single(b => b.Label == "merge");
        var phi = Assert.IsType<IrPhi>(merge.Instructions[0]);
        Assert.True(phi.Sources.ContainsKey("left"));
        Assert.True(phi.Sources.ContainsKey("right"));
    }

    [Fact]
    public void ConditionRegisterPhiAllowsImplicitLiveIn()
    {
        var blocks = new List<IrBasicBlock>
        {
            new("entry", new List<IrInstruction>
            {
                new IrBranch("bne", "left", "right")
            }),
            new("left", new List<IrInstruction>
            {
                new IrBinary("cr1", IrValue.Register("r3"), IrValue.Imm(0), "sub"),
                new IrJump("merge")
            }),
            new("right", new List<IrInstruction>
            {
                new IrJump("merge")
            }),
            new("merge", new List<IrInstruction>
            {
                new IrBranch("eq", "exit_true", "exit_false", "cr1")
            }),
            new("exit_true", new List<IrInstruction>
            {
                new IrReturn(null)
            }),
            new("exit_false", new List<IrInstruction>
            {
                new IrReturn(null)
            })
        };

        var func = new IrFunction("cr_phi", "entry", blocks);
        var ssa = new SsaTransformer().Convert(func);
        ssa.ValidateUseDef();

        var merge = ssa.Function.Blocks.Single(b => b.Label == "merge");
        var phi = Assert.IsType<IrPhi>(merge.Instructions[0]);
        Assert.True(phi.Sources.ContainsKey("left"));
        Assert.True(phi.Sources.ContainsKey("right"));
    }

    [Theory]
    [InlineData("gqr0")]
    [InlineData("gqr7")]
    [InlineData("hid0")]
    [InlineData("hid1")]
    [InlineData("hid2")]
    [InlineData("srr0")]
    [InlineData("srr1")]
    public void ContextBackedSpecialRegisterPhiAllowsImplicitLiveIn(string register)
    {
        var function = new IrFunction("special_phi", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBranch("bne", "written", "live_in")
            }),
            new IrBasicBlock("written", new IrInstruction[]
            {
                new IrAssign(register, IrValue.Imm(1)),
                new IrJump("merge")
            }),
            new IrBasicBlock("live_in", new IrInstruction[]
            {
                new IrJump("merge")
            }),
            new IrBasicBlock("merge", new IrInstruction[]
            {
                new IrReturn(IrValue.Register(register))
            })
        });

        var ssa = new SsaTransformer().Convert(function);
        ssa.ValidateUseDef();

        var phi = Assert.IsType<IrPhi>(
            ssa.Function.Blocks.Single(block => block.Label == "merge").Instructions[0]);
        Assert.Equal(2, phi.Sources.Count);
        Assert.Contains("written", phi.Sources.Keys);
        Assert.Contains("live_in", phi.Sources.Keys);
    }

    [Fact]
    public void SetCrOperandsAreRenamed()
    {
        var blocks = new List<IrBasicBlock>
        {
            new("entry", new List<IrInstruction>
            {
                new IrAssign("r3", IrValue.Imm(1)),
                new IrAssign("r4", IrValue.Imm(2)),
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Register("r4"), true),
                new IrReturn(IrValue.Register("r3"))
            })
        };

        var func = new IrFunction("set_cr", "entry", blocks);
        var ssa = new SsaTransformer().Convert(func);

        var block = ssa.Function.Blocks.Single();
        var setCr = Assert.IsType<IrSetCrField>(block.Instructions[2]);
        Assert.NotNull(setCr.Left.RegisterName);
        Assert.NotNull(setCr.Right.RegisterName);
        Assert.Contains('_', setCr.Left.RegisterName!);
        Assert.Contains('_', setCr.Right.RegisterName!);
    }

    [Fact]
    public void SiblingDefinitionsReceiveGloballyUniqueVersions()
    {
        var function = new IrFunction("sibling_versions", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBranch("bne", "left", "right")
            }),
            new IrBasicBlock("left", new IrInstruction[]
            {
                new IrAssign("f1", IrValue.Imm(1)),
                new IrJump("merge")
            }),
            new IrBasicBlock("right", new IrInstruction[]
            {
                new IrAssign("f1", IrValue.Imm(2)),
                new IrJump("merge")
            }),
            new IrBasicBlock("merge", new IrInstruction[]
            {
                new IrReturn(IrValue.Register("f1"))
            })
        });

        var ssa = new SsaTransformer().Convert(function);
        ssa.ValidateUseDef();

        var leftDefinition = Assert.IsType<IrAssign>(
            ssa.Function.Blocks.Single(block => block.Label == "left").Instructions[0]).Destination;
        var rightDefinition = Assert.IsType<IrAssign>(
            ssa.Function.Blocks.Single(block => block.Label == "right").Instructions[0]).Destination;
        var phi = Assert.IsType<IrPhi>(
            ssa.Function.Blocks.Single(block => block.Label == "merge").Instructions[0]);

        Assert.NotEqual(leftDefinition, rightDefinition);
        Assert.Equal(leftDefinition, phi.Sources["left"]);
        Assert.Equal(rightDefinition, phi.Sources["right"]);
    }
}
