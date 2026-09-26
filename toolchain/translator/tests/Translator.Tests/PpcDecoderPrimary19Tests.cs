using System;
using Translator.Core.Disassembly;
using Xunit;

namespace Translator.Tests;

public class PpcDecoderPrimary19Tests
{
    private static uint EncodePrimary19(uint field0, uint field1, uint field2, uint xo, bool lk = false)
        => (19u << 26) | (field0 << 21) | (field1 << 16) | (field2 << 11) | (xo << 1) | (lk ? 1u : 0u);

    private static PpcInstruction Decode(uint field0, uint field1, uint field2, uint xo, bool lk = false)
        => PpcDecoder.Decode(0x80000000, EncodePrimary19(field0, field1, field2, xo, lk));

    [Fact]
    public void DecodesBclrVariantsWithCorrectFlags()
    {
        var blr = Decode(20, 0, 0, 16);
        Assert.Equal("blr", blr.Mnemonic);
        Assert.True(blr.IsReturn);
        Assert.False(blr.IsCall);
        Assert.False(blr.IsConditionalBranch);
        Assert.Empty(blr.Operands);

        var blrl = Decode(20, 0, 0, 16, lk: true);
        Assert.Equal("blrl", blrl.Mnemonic);
        Assert.False(blrl.IsReturn);
        Assert.True(blrl.IsCall);
        Assert.False(blrl.IsConditionalBranch);
        Assert.Empty(blrl.Operands);

        var beqlr = Decode(12, 6, 0, 16);
        Assert.Equal("beqlr", beqlr.Mnemonic);
        Assert.True(beqlr.IsReturn);
        Assert.True(beqlr.IsConditionalBranch);
        Assert.Equal("cr1", Assert.IsType<PpcConditionRegisterOperand>(Assert.Single(beqlr.Operands)).Name);

        var bnslrl = Decode(4, 7, 0, 16, lk: true);
        Assert.Equal("bnslrl", bnslrl.Mnemonic);
        Assert.False(bnslrl.IsReturn);
        Assert.True(bnslrl.IsCall);
        Assert.True(bnslrl.IsConditionalBranch);
        Assert.Equal("cr1", Assert.IsType<PpcConditionRegisterOperand>(Assert.Single(bnslrl.Operands)).Name);
    }

    [Fact]
    public void DecodesBcctrVariantsAndCrFieldOperands()
    {
        var bctr = Decode(20, 0, 0, 528);
        Assert.Equal("bctr", bctr.Mnemonic);
        Assert.True(bctr.IsReturn);
        Assert.False(bctr.IsCall);
        Assert.False(bctr.IsConditionalBranch);
        Assert.Empty(bctr.Operands);

        var bctrl = Decode(20, 0, 0, 528, lk: true);
        Assert.Equal("bctrl", bctrl.Mnemonic);
        Assert.False(bctrl.IsReturn);
        Assert.True(bctrl.IsCall);
        Assert.False(bctrl.IsConditionalBranch);
        Assert.Empty(bctrl.Operands);

        var bcctr = Decode(4, 9, 0, 528);
        Assert.Equal("bcctr", bcctr.Mnemonic);
        Assert.False(bcctr.IsReturn);
        Assert.False(bcctr.IsCall);
        Assert.True(bcctr.IsConditionalBranch);
        Assert.Equal("cr2", Assert.IsType<PpcConditionRegisterOperand>(Assert.Single(bcctr.Operands)).Name);
    }

    [Fact]
    public void DecodesSystemAndCrMoveForms()
    {
        var rfi = Decode(0, 0, 0, 50);
        Assert.Equal("rfi", rfi.Mnemonic);
        Assert.True(rfi.IsReturn);

        var isync = Decode(0, 0, 0, 150);
        Assert.Equal("isync", isync.Mnemonic);
        Assert.False(isync.IsReturn);

        var mcrf = Decode(12, 4, 0, 0);
        Assert.Equal("mcrf", mcrf.Mnemonic);
        Assert.Collection(
            mcrf.Operands,
            op => Assert.Equal("cr3", Assert.IsType<PpcConditionRegisterOperand>(op).Name),
            op => Assert.Equal("cr1", Assert.IsType<PpcConditionRegisterOperand>(op).Name));
    }

    [Theory]
    [InlineData(33u, "crnor")]
    [InlineData(129u, "crandc")]
    [InlineData(193u, "crxor")]
    [InlineData(225u, "crnand")]
    [InlineData(257u, "crand")]
    [InlineData(289u, "creqv")]
    [InlineData(417u, "crorc")]
    [InlineData(449u, "cror")]
    public void DecodesCrLogicalOps(uint xo, string expectedMnemonic)
    {
        var instruction = Decode(1, 2, 3, xo);

        Assert.Equal(expectedMnemonic, instruction.Mnemonic);
        Assert.Collection(
            instruction.Operands,
            op =>
            {
                var cr = Assert.IsType<PpcConditionRegisterOperand>(op);
                Assert.Equal("crb1", cr.Name);
                Assert.Equal(1, cr.BitIndex);
            },
            op =>
            {
                var cr = Assert.IsType<PpcConditionRegisterOperand>(op);
                Assert.Equal("crb2", cr.Name);
                Assert.Equal(2, cr.BitIndex);
            },
            op =>
            {
                var cr = Assert.IsType<PpcConditionRegisterOperand>(op);
                Assert.Equal("crb3", cr.Name);
                Assert.Equal(3, cr.BitIndex);
            });
    }

    [Fact]
    public void UnknownPrimary19XoFallsBackToUnk19()
    {
        var instruction = Decode(0, 0, 0, 511);

        Assert.Equal("unk19", instruction.Mnemonic);
        Assert.Empty(instruction.Operands);
        Assert.False(instruction.IsReturn);
        Assert.False(instruction.IsCall);
    }
}
