using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Xunit;

namespace Translator.Tests;

public class PairedSingleLifterTests
{
    private static PpcRegisterOperand Fpr(int index) => new($"f{index}", index);
    private static PpcRegisterOperand Gpr(int index) => new($"r{index}", index);

    [Fact]
    public void LiftsPsSum0UsingPpcHelperOperandOrder()
    {
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "ps_sum0",
            new PpcOperand[] { Fpr(1), Fpr(2), Fpr(3), Fpr(4) });

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var call = Assert.IsType<IrCall>(Assert.Single(ir));
        Assert.Equal("f1", call.Destination);
        Assert.Equal("PPC_PsSum0", call.Target);
        Assert.Equal(new[] { "f2", "f4", "f3" }, call.Arguments.Select(a => a.RegisterName));
    }

    [Fact]
    public void LiftsPsSelWithControlInSecondArgument()
    {
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "ps_sel",
            new PpcOperand[] { Fpr(1), Fpr(2), Fpr(3), Fpr(4) });

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var call = Assert.IsType<IrCall>(Assert.Single(ir));
        Assert.Equal("PPC_PsSel", call.Target);
        Assert.Equal(new[] { "f3", "f2", "f4" }, call.Arguments.Select(a => a.RegisterName));
    }

    [Fact]
    public void LiftsRawOpcodeAliasForPsMul()
    {
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            0x106100B2u,
            "opc_4_50",
            System.Array.Empty<PpcOperand>());

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var call = Assert.IsType<IrCall>(Assert.Single(ir));
        Assert.Equal("PPC_PsMul", call.Target);
        Assert.Equal("f3", call.Destination);
        Assert.Equal(new[] { "f1", "f2" }, call.Arguments.Select(a => a.RegisterName));
    }

    [Fact]
    public void LiftsPsNegFromExplicitMnemonic()
    {
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "ps_neg",
            new PpcOperand[] { Fpr(4), Fpr(7) });

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var call = Assert.IsType<IrCall>(Assert.Single(ir));
        Assert.Equal("f4", call.Destination);
        Assert.Equal("PPC_PsNeg", call.Target);
        Assert.Equal("f7", Assert.Single(call.Arguments).RegisterName);
    }

    [Fact]
    public void LiftsPsqLoadWithAbsoluteBaseZeroAddress()
    {
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "psq_l",
            new PpcOperand[]
            {
                Fpr(2),
                new PpcDisplacementOperand(0x24, "r0", 0),
                new PpcImmediateOperand(1),
                new PpcImmediateOperand(3)
            });

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        Assert.Equal(3, ir.Count);
        Assert.IsType<IrComment>(ir[0]);
        var assign = Assert.IsType<IrAssign>(ir[1]);
        Assert.Equal("f2_psq_ea", assign.Destination);
        Assert.Equal(0x24, assign.Value.Constant);
        var call = Assert.IsType<IrCall>(ir[2]);
        Assert.Equal("PPC_PsqL", call.Target);
        Assert.Equal("f2", call.Destination);
        Assert.Equal(new long?[] { null, 1, 3 }, call.Arguments.Select(a => a.Constant).ToArray());
    }

    [Fact]
    public void LiftsPsqStoreUpdateFormAndWritesBackBase()
    {
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "psq_stu",
            new PpcOperand[]
            {
                Fpr(2),
                new PpcDisplacementOperand(0x10, "r4", 4),
                new PpcImmediateOperand(0),
                new PpcImmediateOperand(6)
            });

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        Assert.Equal(4, ir.Count);
        Assert.IsType<IrComment>(ir[0]);
        var addr = Assert.IsType<IrBinary>(ir[1]);
        Assert.Equal("r4_psq_ea", addr.Destination);
        var store = Assert.IsType<IrCall>(ir[2]);
        Assert.Equal("PPC_PsqSt", store.Target);
        Assert.Equal("r4_psq_ea", store.Arguments[0].RegisterName);
        Assert.Equal("f2", store.Arguments[1].RegisterName);
        var writeback = Assert.IsType<IrAssign>(ir[3]);
        Assert.Equal("r4", writeback.Destination);
        Assert.Equal("r4_psq_ea", writeback.Value.RegisterName);
    }

    [Fact]
    public void RejectsInvalidPsqUpdateWithR0Base()
    {
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "psq_lu",
            new PpcOperand[]
            {
                Fpr(1),
                new PpcDisplacementOperand(4, "r0", 0),
                new PpcImmediateOperand(0),
                new PpcImmediateOperand(0)
            });

        Assert.Throws<InvalidOperationException>(() => new PpcLifter().Lift(new[] { instruction }));
    }

    [Fact]
    public void LiftsPsCompareUsingCrFieldImmediate()
    {
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "ps_cmpu1",
            new PpcOperand[]
            {
                new PpcConditionRegisterOperand("cr7", 28),
                Fpr(2),
                Fpr(3)
            });

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var call = Assert.IsType<IrCall>(Assert.Single(ir));
        Assert.Equal("PPC_PsCmpu1", call.Target);
        Assert.Equal(7, call.Arguments[0].Constant);
        Assert.Equal("f2", call.Arguments[1].RegisterName);
        Assert.Equal("f3", call.Arguments[2].RegisterName);
    }

    [Fact]
    public void LiftsPsSelAliasAsPsNegWhenRawXformMatches()
    {
        var raw = (4u << 21) | (7u << 11) | (40u << 1);
        var instruction = PpcInstruction.Synthetic(
            0x80000000,
            raw,
            "ps_sel",
            new PpcOperand[] { Fpr(4), Fpr(1), Fpr(2), Fpr(3) });

        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;
        var call = Assert.IsType<IrCall>(Assert.Single(ir));
        Assert.Equal("PPC_PsNeg", call.Target);
        Assert.Equal("f4", call.Destination);
        Assert.Equal("f7", Assert.Single(call.Arguments).RegisterName);
    }

    [Fact]
    public void LiftsRawOpcodeVariantsForMaddsAndIndexedLoadStore()
    {
        var maddsRaw = (3u << 21) | (1u << 16) | (2u << 11) | (4u << 6);
        var madds = PpcInstruction.Synthetic(0x80000000, maddsRaw, "opc_4_28", System.Array.Empty<PpcOperand>());
        var maddsIr = Assert.Single(new PpcLifter().Lift(new[] { madds })).Ir;
        var maddsCall = Assert.IsType<IrCall>(Assert.Single(maddsIr));
        Assert.Equal("PPC_PsMadds0", maddsCall.Target);
        Assert.Equal("f3", maddsCall.Destination);
        Assert.Equal(new[] { "f1", "f4", "f2" }, maddsCall.Arguments.Select(a => a.RegisterName));

        var loadRaw = (3u << 21) | (4u << 16) | (5u << 11) | (1u << 10) | (6u << 7);
        var load = PpcInstruction.Synthetic(0x80000004, loadRaw, "opc_4_76", System.Array.Empty<PpcOperand>());
        var loadIr = Assert.Single(new PpcLifter().Lift(new[] { load })).Ir;
        Assert.Equal(3, loadIr.Count);
        var loadCall = Assert.IsType<IrCall>(loadIr[1]);
        Assert.Equal("PPC_PsqL", loadCall.Target);
        Assert.Equal("f3", loadCall.Destination);
        Assert.Equal(1, loadCall.Arguments[1].Constant);
        Assert.Equal(6, loadCall.Arguments[2].Constant);
        var loadWriteback = Assert.IsType<IrAssign>(loadIr[2]);
        Assert.Equal("r4", loadWriteback.Destination);

        var storeRaw = (2u << 21) | (4u << 16) | (5u << 11) | (1u << 10) | (3u << 7);
        var store = PpcInstruction.Synthetic(0x80000008, storeRaw, "opc_4_78", System.Array.Empty<PpcOperand>());
        var storeIr = Assert.Single(new PpcLifter().Lift(new[] { store })).Ir;
        Assert.Equal(3, storeIr.Count);
        var storeCall = Assert.IsType<IrCall>(storeIr[1]);
        Assert.Equal("PPC_PsqSt", storeCall.Target);
        Assert.Equal("f2", storeCall.Arguments[1].RegisterName);
        Assert.Equal(1, storeCall.Arguments[2].Constant);
        Assert.Equal(3, storeCall.Arguments[3].Constant);
        var storeWriteback = Assert.IsType<IrAssign>(storeIr[2]);
        Assert.Equal("r4", storeWriteback.Destination);
    }

    [Fact]
    public void LiftsMergeAndScalarEstimateHelpers()
    {
        var merge = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "ps_merge10",
            new PpcOperand[] { Fpr(1), Fpr(2), Fpr(3) });
        var mergeIr = Assert.Single(new PpcLifter().Lift(new[] { merge })).Ir;
        var mergeCall = Assert.IsType<IrCall>(Assert.Single(mergeIr));
        Assert.Equal("PPC_PsMerge10", mergeCall.Target);

        var res = PpcInstruction.Synthetic(
            0x80000004,
            0,
            "ps_rsqrte",
            new PpcOperand[] { Fpr(4), Fpr(5) });
        var resIr = Assert.Single(new PpcLifter().Lift(new[] { res })).Ir;
        var resCall = Assert.IsType<IrCall>(Assert.Single(resIr));
        Assert.Equal("PPC_PsRsqrte", resCall.Target);
        Assert.Equal("f4", resCall.Destination);

        var nabs = PpcInstruction.Synthetic(
            0x80000008,
            0,
            "ps_nabs",
            new PpcOperand[] { Fpr(6), Fpr(7) });
        var nabsIr = Assert.Single(new PpcLifter().Lift(new[] { nabs })).Ir;
        var nabsCall = Assert.IsType<IrCall>(Assert.Single(nabsIr));
        Assert.Equal("PPC_PsNabs", nabsCall.Target);
        Assert.Equal("f6", nabsCall.Destination);
    }

    [Fact]
    public void KeepsPairedMoveDistinctFromScalarFmr()
    {
        var move = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "ps_mr",
            new PpcOperand[] { Fpr(2), Fpr(7) });
        var moveIr = Assert.Single(new PpcLifter().Lift(new[] { move })).Ir;
        var moveCall = Assert.IsType<IrCall>(Assert.Single(moveIr));

        Assert.Equal("PPC_PsMr", moveCall.Target);
        Assert.Equal("f2", moveCall.Destination);
        Assert.Equal("f7", Assert.Single(moveCall.Arguments).RegisterName);
    }

    [Fact]
    public void LiftsDotVariantsForPairedSingleArithmetic()
    {
        var div = PpcInstruction.Synthetic(
            0x7FFFFFFC,
            0,
            "ps_div.",
            new PpcOperand[] { Fpr(1), Fpr(2), Fpr(3) });
        var divIr = Assert.Single(new PpcLifter().Lift(new[] { div })).Ir;
        var divCall = Assert.IsType<IrCall>(Assert.Single(divIr));
        Assert.Equal("PPC_PsDiv", divCall.Target);
        Assert.Equal(new[] { "f2", "f3" }, divCall.Arguments.Select(a => a.RegisterName));

        var madds0 = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "ps_madds0.",
            new PpcOperand[] { Fpr(1), Fpr(2), Fpr(3), Fpr(4) });
        var madds0Ir = Assert.Single(new PpcLifter().Lift(new[] { madds0 })).Ir;
        Assert.Equal("PPC_PsMadds0", Assert.IsType<IrCall>(Assert.Single(madds0Ir)).Target);

        var madds1 = PpcInstruction.Synthetic(
            0x80000004,
            0,
            "ps_madds1.",
            new PpcOperand[] { Fpr(1), Fpr(2), Fpr(3), Fpr(4) });
        var madds1Ir = Assert.Single(new PpcLifter().Lift(new[] { madds1 })).Ir;
        var madds1Call = Assert.IsType<IrCall>(Assert.Single(madds1Ir));
        Assert.Equal("PPC_PsMadds1", madds1Call.Target);
        Assert.Equal(new[] { "f2", "f3", "f4" }, madds1Call.Arguments.Select(a => a.RegisterName));

        var sum0 = PpcInstruction.Synthetic(
            0x80000008,
            0,
            "ps_sum0.",
            new PpcOperand[] { Fpr(5), Fpr(6), Fpr(7), Fpr(8) });
        var sum0Ir = Assert.Single(new PpcLifter().Lift(new[] { sum0 })).Ir;
        var sum0Call = Assert.IsType<IrCall>(Assert.Single(sum0Ir));
        Assert.Equal("PPC_PsSum0", sum0Call.Target);
        Assert.Equal(new[] { "f6", "f8", "f7" }, sum0Call.Arguments.Select(a => a.RegisterName));

        var sum1 = PpcInstruction.Synthetic(
            0x8000000C,
            0,
            "ps_sum1.",
            new PpcOperand[] { Fpr(5), Fpr(6), Fpr(7), Fpr(8) });
        var sum1Ir = Assert.Single(new PpcLifter().Lift(new[] { sum1 })).Ir;
        var sum1Call = Assert.IsType<IrCall>(Assert.Single(sum1Ir));
        Assert.Equal("PPC_PsSum1", sum1Call.Target);
        Assert.Equal(new[] { "f6", "f8", "f7" }, sum1Call.Arguments.Select(a => a.RegisterName));

        var muls0 = PpcInstruction.Synthetic(
            0x80000008,
            0,
            "ps_muls0.",
            new PpcOperand[] { Fpr(9), Fpr(10), Fpr(11) });
        var muls0Ir = Assert.Single(new PpcLifter().Lift(new[] { muls0 })).Ir;
        Assert.Equal("PPC_PsMuls0", Assert.IsType<IrCall>(Assert.Single(muls0Ir)).Target);

        var muls1 = PpcInstruction.Synthetic(
            0x8000000C,
            0,
            "ps_muls1.",
            new PpcOperand[] { Fpr(12), Fpr(13), Fpr(14) });
        var muls1Ir = Assert.Single(new PpcLifter().Lift(new[] { muls1 })).Ir;
        Assert.Equal("PPC_PsMuls1", Assert.IsType<IrCall>(Assert.Single(muls1Ir)).Target);
    }

    [Fact]
    public void LiftsExplicitUnaryMoveAndCompareVariants()
    {
        var res = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "ps_res",
            new PpcOperand[] { Fpr(1), Fpr(2) });
        var resIr = Assert.Single(new PpcLifter().Lift(new[] { res })).Ir;
        Assert.Equal("PPC_PsRes", Assert.IsType<IrCall>(Assert.Single(resIr)).Target);

        var abs = PpcInstruction.Synthetic(
            0x80000004,
            0,
            "ps_abs.",
            new PpcOperand[] { Fpr(3), Fpr(4) });
        var absIr = Assert.Single(new PpcLifter().Lift(new[] { abs })).Ir;
        Assert.Equal("PPC_PsAbs", Assert.IsType<IrCall>(Assert.Single(absIr)).Target);

        var mr = PpcInstruction.Synthetic(
            0x80000008,
            0,
            "ps_mr",
            new PpcOperand[] { Fpr(5), Fpr(6) });
        var mrIr = Assert.Single(new PpcLifter().Lift(new[] { mr })).Ir;
        var mrCall = Assert.IsType<IrCall>(Assert.Single(mrIr));
        Assert.Equal("PPC_PsMr", mrCall.Target);
        Assert.Equal("f5", mrCall.Destination);
        Assert.Equal("f6", Assert.Single(mrCall.Arguments).RegisterName);

        var mrDot = PpcInstruction.Synthetic(
            0x8000000A,
            0,
            "ps_mr.",
            new PpcOperand[] { Fpr(5), Fpr(6) });
        Assert.Equal(
            "PPC_PsMr",
            Assert.IsType<IrCall>(Assert.Single(Assert.Single(new PpcLifter().Lift(new[] { mrDot })).Ir)).Target);

        var cmpo0 = PpcInstruction.Synthetic(
            0x8000000C,
            0,
            "ps_cmpo0.",
            new PpcOperand[] { new PpcConditionRegisterOperand("cr5", 20), Fpr(7), Fpr(8) });
        var cmpo0Ir = Assert.Single(new PpcLifter().Lift(new[] { cmpo0 })).Ir;
        var cmpo0Call = Assert.IsType<IrCall>(Assert.Single(cmpo0Ir));
        Assert.Equal("PPC_PsCmpo0", cmpo0Call.Target);
        Assert.Equal(5, cmpo0Call.Arguments[0].Constant);

        var cmpu0 = PpcInstruction.Synthetic(
            0x80000010,
            0,
            "ps_cmpu0",
            new PpcOperand[] { new PpcConditionRegisterOperand("cr3", 12), Fpr(9), Fpr(10) });
        var cmpu0Ir = Assert.Single(new PpcLifter().Lift(new[] { cmpu0 })).Ir;
        var cmpu0Call = Assert.IsType<IrCall>(Assert.Single(cmpu0Ir));
        Assert.Equal("PPC_PsCmpu0", cmpu0Call.Target);
        Assert.Equal(3, cmpu0Call.Arguments[0].Constant);

        var cmpo1 = PpcInstruction.Synthetic(
            0x80000014,
            0,
            "ps_cmpo1",
            new PpcOperand[] { new PpcConditionRegisterOperand("cr2", 8), Fpr(11), Fpr(12) });
        var cmpo1Ir = Assert.Single(new PpcLifter().Lift(new[] { cmpo1 })).Ir;
        var cmpo1Call = Assert.IsType<IrCall>(Assert.Single(cmpo1Ir));
        Assert.Equal("PPC_PsCmpo1", cmpo1Call.Target);
        Assert.Equal(2, cmpo1Call.Arguments[0].Constant);

        var cmpu1 = PpcInstruction.Synthetic(
            0x80000018,
            0,
            "ps_cmpu1.",
            new PpcOperand[] { new PpcConditionRegisterOperand("cr1", 4), Fpr(13), Fpr(14) });
        var cmpu1Ir = Assert.Single(new PpcLifter().Lift(new[] { cmpu1 })).Ir;
        var cmpu1Call = Assert.IsType<IrCall>(Assert.Single(cmpu1Ir));
        Assert.Equal("PPC_PsCmpu1", cmpu1Call.Target);
        Assert.Equal(1, cmpu1Call.Arguments[0].Constant);
    }

    [Fact]
    public void LiftsRawOpc4AliasesForCompareMoveAndIndexedMemory()
    {
        var compare = PpcInstruction.Synthetic(
            0x80000000,
            (1u << 16) | (2u << 11),
            "opc_4_0",
            System.Array.Empty<PpcOperand>());
        var compareIr = Assert.Single(new PpcLifter().Lift(new[] { compare })).Ir;
        var compareSetCr = Assert.IsType<IrSetCrField>(Assert.Single(compareIr));
        Assert.Equal(0, compareSetCr.FieldIndex);
        Assert.Equal("f1", compareSetCr.Left.RegisterName);
        Assert.Equal("f2", compareSetCr.Right.RegisterName);

        var move = PpcInstruction.Synthetic(
            0x80000004,
            (4u << 21) | (7u << 11),
            "opc_4_4",
            System.Array.Empty<PpcOperand>());
        var moveIr = Assert.Single(new PpcLifter().Lift(new[] { move })).Ir;
        var moveCall = Assert.IsType<IrCall>(Assert.Single(moveIr));
        Assert.Equal("PPC_PsMr", moveCall.Target);
        Assert.Equal("f4", moveCall.Destination);
        Assert.Equal("f7", Assert.Single(moveCall.Arguments).RegisterName);

        var neg = PpcInstruction.Synthetic(
            0x80000008,
            (5u << 21) | (8u << 11),
            "opc_4_5",
            System.Array.Empty<PpcOperand>());
        var negIr = Assert.Single(new PpcLifter().Lift(new[] { neg })).Ir;
        var negCall = Assert.IsType<IrCall>(Assert.Single(negIr));
        Assert.Equal("PPC_PsNabs", negCall.Target);
        Assert.Equal("f5", negCall.Destination);
        Assert.Equal("f8", Assert.Single(negCall.Arguments).RegisterName);

        var load = PpcInstruction.Synthetic(
            0x8000000C,
            (3u << 21) | (4u << 16) | (5u << 11) | (1u << 10) | (6u << 7),
            "opc_4_12",
            System.Array.Empty<PpcOperand>());
        var loadIr = Assert.Single(new PpcLifter().Lift(new[] { load })).Ir;
        Assert.Equal(2, loadIr.Count);
        var loadCall = Assert.IsType<IrCall>(loadIr[1]);
        Assert.Equal("PPC_PsqL", loadCall.Target);
        Assert.Equal("f3", loadCall.Destination);
        Assert.Equal(1, loadCall.Arguments[1].Constant);
        Assert.Equal(6, loadCall.Arguments[2].Constant);

        var store = PpcInstruction.Synthetic(
            0x80000010,
            (2u << 21) | (4u << 16) | (5u << 11) | (1u << 10) | (3u << 7),
            "opc_4_14",
            System.Array.Empty<PpcOperand>());
        var storeIr = Assert.Single(new PpcLifter().Lift(new[] { store })).Ir;
        Assert.Equal(2, storeIr.Count);
        var storeCall = Assert.IsType<IrCall>(storeIr[1]);
        Assert.Equal("PPC_PsqSt", storeCall.Target);
        Assert.Equal("f2", storeCall.Arguments[1].RegisterName);
        Assert.Equal(1, storeCall.Arguments[2].Constant);
        Assert.Equal(3, storeCall.Arguments[3].Constant);
    }

    [Fact]
    public void RejectsMalformedAndUnknownRawOpc4Variants()
    {
        var malformed = PpcInstruction.Synthetic(
            0x80000000,
            0,
            "opc_4_bad",
            System.Array.Empty<PpcOperand>());
        Assert.Throws<NotImplementedException>(() => new PpcLifter().Lift(new[] { malformed }));

        var unknown = PpcInstruction.Synthetic(
            0x80000004,
            0,
            "opc_4_999",
            System.Array.Empty<PpcOperand>());
        Assert.Throws<NotImplementedException>(() => new PpcLifter().Lift(new[] { unknown }));
    }

    [Theory]
    [InlineData("opc_4_20", "call", "PPC_PsSum0")]
    [InlineData("opc_4_22", "call", "PPC_PsSum1")]
    [InlineData("opc_4_24", "call", "PPC_PsMuls0")]
    [InlineData("opc_4_26", "call", "PPC_PsMuls1")]
    [InlineData("opc_4_30", "call", "PPC_PsMadds1")]
    [InlineData("opc_4_36", "call", "PPC_PsDiv")]
    [InlineData("opc_4_40", "call", "PPC_PsSub")]
    [InlineData("opc_4_42", "call", "PPC_PsAdd")]
    [InlineData("opc_4_44", "call", "PPC_PsAbs")]
    [InlineData("opc_4_46", "call", "PPC_PsSel")]
    [InlineData("opc_4_48", "call", "PPC_PsRes")]
    [InlineData("opc_4_52", "call", "PPC_PsRsqrte")]
    [InlineData("opc_4_56", "call", "PPC_PsMsub")]
    [InlineData("opc_4_58", "call", "PPC_PsMadd")]
    [InlineData("opc_4_60", "call", "PPC_PsNmsub")]
    [InlineData("opc_4_62", "call", "PPC_PsNmadd")]
    public void LiftsAdditionalRawOpc4ArithmeticVariants(string mnemonic, string kind, string opOrTarget)
    {
        var raw = (3u << 21) | (1u << 16) | (2u << 11) | (4u << 6);
        var instruction = PpcInstruction.Synthetic(0x80000000, raw, mnemonic, System.Array.Empty<PpcOperand>());
        var ir = Assert.Single(new PpcLifter().Lift(new[] { instruction })).Ir;

        if (kind == "call")
        {
            var call = Assert.IsType<IrCall>(Assert.Single(ir));
            Assert.Equal(opOrTarget, call.Target);
            Assert.Equal("f3", call.Destination);
        }
        else
        {
            var binary = Assert.IsType<IrBinary>(Assert.Single(ir));
            Assert.Equal(opOrTarget, binary.Op);
            Assert.Equal("f3", binary.Destination);
        }
    }
}
