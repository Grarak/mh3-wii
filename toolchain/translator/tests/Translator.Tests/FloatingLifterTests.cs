using System;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

public class FloatingLifterTests
{
    private static PpcRegisterOperand Fpr(int index) => new($"f{index}", index);

    private static PpcInstruction FloatingInstruction(uint raw, string mnemonic, params PpcOperand[] operands)
        => PpcInstruction.Synthetic(0x80000000, raw, mnemonic, operands);

    [Fact]
    public void LiftsFloatingStatusRegisterHelpersFromRawBits()
    {
        var lifter = new PpcLifter();

        var mffs = FloatingInstruction((63u << 26) | (5u << 21), "mffs");
        var mffsIr = Assert.Single(lifter.Lift(new[] { mffs })).Ir;
        var mffsCall = Assert.IsType<IrCall>(Assert.Single(mffsIr));
        Assert.Equal("PPC_Mffs", mffsCall.Target);
        Assert.Equal("f5", mffsCall.Destination);
        Assert.Empty(mffsCall.Arguments);

        var mtfsf = FloatingInstruction((63u << 26) | (0xAAu << 17) | (7u << 11), "mtfsf");
        var mtfsfIr = Assert.Single(lifter.Lift(new[] { mtfsf })).Ir;
        var mtfsfCall = Assert.IsType<IrCall>(Assert.Single(mtfsfIr));
        Assert.Equal("PPC_Mtfsf", mtfsfCall.Target);
        Assert.Equal(0xAA, mtfsfCall.Arguments[0].Constant);
        Assert.Equal("f7", mtfsfCall.Arguments[1].RegisterName);

        var mtfsb1 = FloatingInstruction((63u << 26) | (9u << 21), "mtfsb1");
        var mtfsb1Ir = Assert.Single(lifter.Lift(new[] { mtfsb1 })).Ir;
        var mtfsb1Call = Assert.IsType<IrCall>(Assert.Single(mtfsb1Ir));
        Assert.Equal("PPC_Mtfsb1", mtfsb1Call.Target);
        Assert.Equal(9, Assert.Single(mtfsb1Call.Arguments).Constant);

        var mtfsb0 = FloatingInstruction((63u << 26) | (10u << 21), "mtfsb0");
        var mtfsb0Ir = Assert.Single(lifter.Lift(new[] { mtfsb0 })).Ir;
        var mtfsb0Call = Assert.IsType<IrCall>(Assert.Single(mtfsb0Ir));
        Assert.Equal("PPC_Mtfsb0", mtfsb0Call.Target);
        Assert.Equal(10, Assert.Single(mtfsb0Call.Arguments).Constant);

        var mtfsfi = FloatingInstruction((63u << 26) | (3u << 23) | (0xCu << 12), "mtfsfi");
        var mtfsfiIr = Assert.Single(lifter.Lift(new[] { mtfsfi })).Ir;
        var mtfsfiCall = Assert.IsType<IrCall>(Assert.Single(mtfsfiIr));
        Assert.Equal("PPC_Mtfsfi", mtfsfiCall.Target);
        Assert.Equal(new long?[] { 3, 0xC }, mtfsfiCall.Arguments.Select(a => a.Constant).ToArray());
    }

    [Theory]
    [InlineData("fadd", "add")]
    [InlineData("fsub", "sub")]
    [InlineData("fmul", "mul")]
    [InlineData("fdiv", "fdiv")]
    public void LiftsFloatingBinaryMnemonics(string mnemonic, string op)
    {
        var instruction = FloatingInstruction(0, mnemonic, Fpr(1), Fpr(2), Fpr(3));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var binary = Assert.IsType<IrBinary>(Assert.Single(ir));
        Assert.Equal("f1", binary.Destination);
        Assert.Equal(op, binary.Op);
        Assert.Equal("f2", binary.Left.RegisterName);
        Assert.Equal("f3", binary.Right.RegisterName);
    }

    [Theory]
    [InlineData("fadd.", "add")]
    [InlineData("fsub.", "sub")]
    [InlineData("fmul.", "mul")]
    [InlineData("fdiv.", "fdiv")]
    public void LiftsFloatingBinaryDotMnemonics(string mnemonic, string op)
    {
        var instruction = FloatingInstruction(0, mnemonic, Fpr(1), Fpr(2), Fpr(3));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var binary = Assert.IsType<IrBinary>(Assert.Single(ir));
        Assert.Equal("f1", binary.Destination);
        Assert.Equal(op, binary.Op);
    }

    [Theory]
    [InlineData("frsp", "frsp")]
    [InlineData("fneg", "fneg")]
    [InlineData("fabs", "fabs")]
    [InlineData("fctiw", "fctiw")]
    [InlineData("fctiwz", "fctiwz")]
    public void LiftsFloatingUnaryMnemonics(string mnemonic, string op)
    {
        var instruction = FloatingInstruction(0, mnemonic, Fpr(4), Fpr(7));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var binary = Assert.IsType<IrBinary>(Assert.Single(ir));
        Assert.Equal("f4", binary.Destination);
        Assert.Equal(op, binary.Op);
        Assert.Equal("f7", binary.Left.RegisterName);
        Assert.Equal(0, binary.Right.Constant);
    }

    [Theory]
    [InlineData("frsp.", "frsp")]
    [InlineData("fneg.", "fneg")]
    [InlineData("fabs.", "fabs")]
    [InlineData("fctiw.", "fctiw")]
    [InlineData("fctiwz.", "fctiwz")]
    public void LiftsFloatingUnaryDotMnemonics(string mnemonic, string op)
    {
        var instruction = FloatingInstruction(0, mnemonic, Fpr(4), Fpr(7));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var binary = Assert.IsType<IrBinary>(Assert.Single(ir));
        Assert.Equal("f4", binary.Destination);
        Assert.Equal(op, binary.Op);
    }

    [Fact]
    public void LiftsFmrAsRegisterAssign()
    {
        var instruction = FloatingInstruction(0, "fmr", Fpr(6), Fpr(2));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var assign = Assert.IsType<IrAssign>(Assert.Single(ir));
        Assert.Equal("f6", assign.Destination);
        Assert.Equal("f2", assign.Value.RegisterName);
    }

    [Fact]
    public void LiftsFnabsAsNegativeAbsoluteValue()
    {
        var instruction = FloatingInstruction(0, "fnabs", Fpr(6), Fpr(2));

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        Assert.Equal("fabs", Assert.IsType<IrBinary>(ir[0]).Op);
        Assert.Equal("fneg", Assert.IsType<IrBinary>(ir[1]).Op);
    }

    [Fact]
    public void LiftsFloatingRawAliasesAndFallbacks()
    {
        var lifter = new PpcLifter();

        var fp57 = FloatingInstruction((63u << 26) | (3u << 21) | (1u << 16) | (4u << 6), "fp_57");
        var fp57Ir = Assert.Single(lifter.Lift(new[] { fp57 })).Ir;
        var fp57Binary = Assert.IsType<IrBinary>(Assert.Single(fp57Ir));
        Assert.Equal("f3", fp57Binary.Destination);
        Assert.Equal("mul", fp57Binary.Op);
        Assert.Equal("f1", fp57Binary.Left.RegisterName);
        Assert.Equal("f4", fp57Binary.Right.RegisterName);

        var fp12 = FloatingInstruction((63u << 26) | (6u << 21) | (9u << 11), "fp_12");
        var fp12Ir = Assert.Single(lifter.Lift(new[] { fp12 })).Ir;
        var fp12Binary = Assert.IsType<IrBinary>(Assert.Single(fp12Ir));
        Assert.Equal("f6", fp12Binary.Destination);
        Assert.Equal("frsp", fp12Binary.Op);
        Assert.Equal("f9", fp12Binary.Left.RegisterName);

        var fp281 = FloatingInstruction((63u << 26) | (8u << 21) | (2u << 16) | (5u << 6), "fp_281");
        var fp281Ir = Assert.Single(lifter.Lift(new[] { fp281 })).Ir;
        var fp281Binary = Assert.IsType<IrBinary>(Assert.Single(fp281Ir));
        Assert.Equal("f8", fp281Binary.Destination);
        Assert.Equal("mul", fp281Binary.Op);
        Assert.Equal("f2", fp281Binary.Left.RegisterName);
        Assert.Equal("f5", fp281Binary.Right.RegisterName);
    }

    [Fact]
    public void LiftsFloatingCompareAndRawOpc59Forms()
    {
        var lifter = new PpcLifter();

        var compare = FloatingInstruction(
            0,
            "fcmpu",
            new PpcOperand[]
            {
                new PpcConditionRegisterOperand("cr6", 24),
                Fpr(2),
                Fpr(3)
            });

        var compareIr = Assert.Single(lifter.Lift(new[] { compare })).Ir;
        var compareSet = Assert.IsType<IrSetCrField>(Assert.Single(compareIr));
        Assert.Equal(6, compareSet.FieldIndex);
        Assert.Equal("f2", compareSet.Left.RegisterName);
        Assert.Equal("f3", compareSet.Right.RegisterName);

        var opc59 = FloatingInstruction((59u << 26) | (4u << 21) | (1u << 16) | (2u << 11) | (3u << 6) | (23u << 1), "opc_59");
        var opc59Ir = Assert.Single(lifter.Lift(new[] { opc59 })).Ir;
        var opc59Call = Assert.IsType<IrCall>(Assert.Single(opc59Ir));
        Assert.Equal("PPC_Fsel", opc59Call.Target);
        Assert.Equal("f4", opc59Call.Destination);
        Assert.Equal(new[] { "f1", "f2", "f3" }, opc59Call.Arguments.Select(a => a.RegisterName));

        var fallback = FloatingInstruction((63u << 26) | (4u << 21) | (1u << 16) | (2u << 11) | (3u << 6) | (28u << 1), "fp_999");
        var fallbackIr = Assert.Single(lifter.Lift(new[] { fallback })).Ir;
        var fallbackCall = Assert.IsType<IrCall>(Assert.Single(fallbackIr));
        Assert.Equal("PPC_Fmsub", fallbackCall.Target);
        Assert.Equal("f4", fallbackCall.Destination);
        Assert.Equal(new[] { "f1", "f3", "f2" }, fallbackCall.Arguments.Select(a => a.RegisterName));
    }

    [Theory]
    [InlineData(22u, "PPC_Fsqrt", "f2")]
    [InlineData(26u, "PPC_Frsqrte", "f2")]
    [InlineData(29u, "PPC_Fmadds", "f1")]
    [InlineData(30u, "PPC_Fnmsubs", "f1")]
    [InlineData(31u, "PPC_Fnmadds", "f1")]
    public void LiftsAdditionalOpc59Branches(uint xo, string target, string expectedFirstArgument)
    {
        var instruction = FloatingInstruction(
            (59u << 26) | (4u << 21) | (1u << 16) | (2u << 11) | (3u << 6) | (xo << 1),
            "opc_59");

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var call = Assert.IsType<IrCall>(Assert.Single(ir));
        Assert.Equal(target, call.Target);
        Assert.Equal("f4", call.Destination);
        Assert.Equal(expectedFirstArgument, call.Arguments[0].RegisterName);
    }

    [Fact]
    public void RejectsUnknownOpc59Subop()
    {
        var instruction = FloatingInstruction((59u << 26) | (3u << 21) | (27u << 1), "opc_59");
        Assert.Throws<NotImplementedException>(() => new PpcLifter().Lift(new[] { instruction }));
    }

    [Theory]
    [InlineData(18u, "binary", "fdiv")]
    [InlineData(20u, "binary", "sub")]
    [InlineData(21u, "binary", "add")]
    [InlineData(22u, "call", "PPC_Fsqrt")]
    [InlineData(23u, "call", "PPC_Fsel")]
    [InlineData(24u, "call", "PPC_Fres")]
    [InlineData(26u, "call", "PPC_Frsqrte")]
    [InlineData(29u, "call", "PPC_Fmadd")]
    [InlineData(30u, "call", "PPC_Fnmsub")]
    [InlineData(31u, "call", "PPC_Fnmadd")]
    [InlineData(12u, "binary", "frsp")]
    [InlineData(8u, "binary", "fcmp")]
    public void LiftsAdditionalFpFallbackBranches(uint xo, string kind, string opOrTarget)
    {
        var instruction = FloatingInstruction(
            (63u << 26) | (6u << 21) | (1u << 16) | (2u << 11) | (3u << 6) | (xo << 1),
            "fp_4242");

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        if (kind == "call")
        {
            var call = Assert.IsType<IrCall>(Assert.Single(ir));
            Assert.Equal(opOrTarget, call.Target);
        }
        else
        {
            var binary = Assert.IsType<IrBinary>(Assert.Single(ir));
            Assert.Equal(opOrTarget, binary.Op);
        }
    }
}
