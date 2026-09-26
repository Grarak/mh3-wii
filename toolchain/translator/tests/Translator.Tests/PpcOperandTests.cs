using Translator.Core.Disassembly;
using Xunit;

namespace Translator.Tests;

public class PpcOperandTests
{
    private sealed record CustomOperand(string Text) : PpcOperand
    {
        public override string ToOperandString() => Text;
        public override string ToString() => base.ToString();
    }

    [Fact]
    public void FormatsImmediateOperandsInDecimalAndHex()
    {
        Assert.Equal("42", new PpcImmediateOperand(42).ToOperandString());
        Assert.Equal("0x2A", new PpcImmediateOperand(42, PreferHex: true).ToOperandString());
        Assert.Equal("-0x2A", new PpcImmediateOperand(-42, PreferHex: true).ToOperandString());
    }

    [Fact]
    public void FormatsDisplacementOperands()
    {
        Assert.Equal("0(r3)", new PpcDisplacementOperand(0, "r3", 3).ToOperandString());
        Assert.Equal("0x20(r4)", new PpcDisplacementOperand(0x20, "r4", 4).ToOperandString());
        Assert.Equal("-0x20(r5)", new PpcDisplacementOperand(-0x20, "r5", 5).ToOperandString());
    }

    [Fact]
    public void FormatsBranchAndConditionOperands()
    {
        Assert.Equal("0x8000ABCD", new PpcBranchTargetOperand(0x8000ABCD).ToOperandString());
        Assert.Equal("cr7", new PpcConditionRegisterOperand("cr7", 28).ToOperandString());
        Assert.Equal("r12", new PpcRegisterOperand("r12", 12).ToOperandString());
    }

    [Fact]
    public void BaseOperandToStringDelegatesToTypedFormatter()
    {
        PpcOperand operand = new CustomOperand("custom");
        Assert.Equal("custom", operand.ToString());
    }
}
