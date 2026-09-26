using System;
using System.Reflection;
using Translator.Core.Analysis.Representation;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class CodeGenExpressionCoverageTests
{
    private static readonly Type GeneratorType = typeof(CxxLinearCodeGenerator);

    private static T InvokePrivate<T>(string name, params object?[] args)
    {
        var method = GeneratorType.GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);
        return (T)method!.Invoke(null, args)!;
    }

    private static string FormatBinary(IrBinary binary, RepresentationEnvironment types)
    {
        var left = InvokePrivate<string>("ToExpression", binary.Left, types);
        var right = InvokePrivate<string>("ToExpression", binary.Right, types);
        return InvokePrivate<string>("BinaryExpressionWithOperands", binary, left, right);
    }

    [Fact]
    public void BinaryExpression_FormatsRareOperations()
    {
        var types = new RepresentationEnvironment();

        Assert.Equal(
            "(static_cast<int32_t>(ctx->gpr[3]))",
            FormatBinary(new IrBinary("r4", IrValue.Register("r3"), IrValue.Imm(12), "sext"), types));
        Assert.Equal(
            "~(ctx->gpr[3] & ctx->gpr[4])",
            FormatBinary(new IrBinary("r5", IrValue.Register("r3"), IrValue.Register("r4"), "nand"), types));
        Assert.Equal(
            "~(ctx->gpr[3] ^ ctx->gpr[4])",
            FormatBinary(new IrBinary("r5", IrValue.Register("r3"), IrValue.Register("r4"), "eqv"), types));
        Assert.Equal(
            "std::sqrt(ctx->fpr[1].d)",
            FormatBinary(new IrBinary("f2", IrValue.Register("f1"), IrValue.Register("f3"), "fsqrt"), types));
        Assert.Equal(
            "(ctx->fpr[1].d - ctx->fpr[2].d)",
            FormatBinary(new IrBinary("f3", IrValue.Register("f1"), IrValue.Register("f2"), "fcmp"), types));
        Assert.Equal(
            "PPC_Divw(static_cast<int32_t>(ctx->gpr[7]), static_cast<int32_t>(ctx->gpr[8]))",
            FormatBinary(new IrBinary("r9", IrValue.Register("r7"), IrValue.Register("r8"), "div"), types));
        Assert.Equal(
            "(static_cast<int32_t>(static_cast<int16_t>(ctx->gpr[9])))",
            FormatBinary(new IrBinary("r10", IrValue.Register("r9"), IrValue.Imm(0), "sext16"), types));
        Assert.Equal(
            "(CompareUnsigned(static_cast<uint32_t>(ctx->gpr[11]), static_cast<uint32_t>(ctx->gpr[12])))",
            FormatBinary(new IrBinary("r13", IrValue.Register("r11"), IrValue.Register("r12"), "sub_u"), types));
        Assert.Equal(
            "PPC_Fctiwz(ctx->fpr[4].d)",
            FormatBinary(new IrBinary("f5", IrValue.Register("f4"), IrValue.Imm(0), "fctiwz"), types));
        Assert.Equal(
            "PPC_Fctiw(ctx->fpr[4].d)",
            FormatBinary(new IrBinary("f5", IrValue.Register("f4"), IrValue.Imm(0), "fctiw"), types));
        Assert.Equal(
            "PPC_Slw(static_cast<uint32_t>(ctx->gpr[3]), static_cast<uint32_t>(ctx->gpr[4]))",
            FormatBinary(new IrBinary("r5", IrValue.Register("r3"), IrValue.Register("r4"), "ppc_slw"), types));
        Assert.Equal(
            "PPC_Srw(static_cast<uint32_t>(ctx->gpr[3]), static_cast<uint32_t>(ctx->gpr[4]))",
            FormatBinary(new IrBinary("r5", IrValue.Register("r3"), IrValue.Register("r4"), "ppc_srw"), types));
        Assert.Equal(
            "PPC_Sraw(static_cast<uint32_t>(ctx->gpr[3]), static_cast<uint32_t>(ctx->gpr[4]))",
            FormatBinary(new IrBinary("r5", IrValue.Register("r3"), IrValue.Register("r4"), "ppc_sraw"), types));
    }

    [Fact]
    public void RegisterHelpers_MapSpecialRegistersAndConditions()
    {
        var types = new RepresentationEnvironment();

        Assert.Equal("ctx->cr", InvokePrivate<string>("RegisterToCtxRead", "cr", types));
        Assert.Equal("((ctx->cr >> 0) & 0xF)", InvokePrivate<string>("RegisterToCtxRead", "cr7", types));
        Assert.Equal("ctx->srr0", InvokePrivate<string>("RegisterToCtxRead", "srr0", types));
        Assert.Equal("ctx->srr1", InvokePrivate<string>("RegisterToCtxRead", "srr1", types));
        Assert.Equal("ctx->hid0", InvokePrivate<string>("RegisterToCtxRead", "hid0", types));
        Assert.Equal("ctx->hid1", InvokePrivate<string>("RegisterToCtxRead", "hid1", types));
        Assert.Equal("ctx->hid2", InvokePrivate<string>("RegisterToCtxRead", "hid2", types));
        Assert.Equal("ctx->gqr[3]", InvokePrivate<string>("RegisterToCtxRead", "gqr3", types));
        Assert.Equal("0", InvokePrivate<string>("RegisterToCtxRead", "gqr9", types));

        Assert.Equal("ctx->cr", InvokePrivate<string>("RegisterToCtxWrite", "cr", types));
        Assert.Equal("cr6", InvokePrivate<string>("RegisterToCtxWrite", "cr6", types));
        Assert.Equal("ctx->hid2", InvokePrivate<string>("RegisterToCtxWrite", "hid2", types));
        Assert.Equal("ctx->gqr[5]", InvokePrivate<string>("RegisterToCtxWrite", "gqr5", types));
        Assert.Equal("gqr8", InvokePrivate<string>("RegisterToCtxWrite", "gqr8", types));

        Assert.Equal("0", InvokePrivate<string>("ToExpression", new IrValue("temporary"), types));
        Assert.Equal("tmp_local", InvokePrivate<string>("ToExpression", IrValue.Register("tmp_local"), types));

        Assert.Equal("(ctx->ctr != 0)", InvokePrivate<string>("ToCondition", "bdnz", "ctr", types));
        Assert.Equal("(ctx->ctr == 0)", InvokePrivate<string>("ToCondition", "bdz", "ctr", types));
        Assert.Equal("((ctx->cr & 0x00000001u) != 0)", InvokePrivate<string>("ToCondition", "bso", "cr7", types));
        Assert.Equal("((ctx->cr & 0x00000001u) == 0)", InvokePrivate<string>("ToCondition", "bns", "cr7", types));
        Assert.Equal("flag_local", InvokePrivate<string>("ToCondition", "custom", "flag_local", types));

        Assert.Equal("ctx->gpr[4]", InvokePrivate<string>("Address", new IrAddress("r4", 0), types));
        Assert.Equal("(ctx->gpr[4] + 16)", InvokePrivate<string>("Address", new IrAddress("r4", 16), types));
        Assert.Equal("r3", InvokePrivate<string>("BaseRegister", "r3_17"));
        Assert.Equal("tmp", InvokePrivate<string>("BaseRegister", "tmp_local"));

        Assert.Equal(8, InvokePrivate<int>("Bits", 1));
        Assert.Equal(16, InvokePrivate<int>("Bits", 2));
        Assert.Equal(64, InvokePrivate<int>("Bits", 8));
        Assert.Equal(32, InvokePrivate<int>("Bits", 4));
    }
}
