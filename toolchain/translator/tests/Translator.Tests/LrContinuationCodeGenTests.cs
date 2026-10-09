using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

// This binary-free code-generation regression must run in the default suite.
public class LrContinuationCodeGenTests
{
    [Fact]
    public void CodeGenerator_DispatchesGuestCallLrContinuationWithoutMarkingTargetNonReturning()
    {
        var function = new IrFunction(
            "lr_continuation_call",
            "0x800E591C",
            new[]
            {
                new IrBasicBlock("0x800E591C", new IrInstruction[]
                {
                    new IrAssign("lr", IrValue.Imm(unchecked((int)0x800E5920u))),
                    new IrCall(string.Empty, "0x8179AC3C", Array.Empty<IrValue>()),
                    new IrAssign("r3", IrValue.Imm(8)),
                    new IrReturn(null)
                }),
                new IrBasicBlock("0x800E5934", new IrInstruction[]
                {
                    new IrAssign("r3", IrValue.Imm(1)),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["lr"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("lr_continuation_call", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x800E591C,
            ssa,
            signature,
            types,
            lrContinuationCallTargets: new HashSet<uint> { 0x8179AC3Cu });

        var callIndex = code.IndexOf("InvokeDirectCpu<0x8179AC3Cu>(ctx);", StringComparison.Ordinal);
        var fallthroughGuardIndex = code.IndexOf("if (ctx->lr != 0x800E5920u)", callIndex, StringComparison.Ordinal);
        var localCaseIndex = code.IndexOf("case 0x800E5934u:", fallthroughGuardIndex, StringComparison.Ordinal);
        var returnIndex = code.IndexOf("return;", localCaseIndex, StringComparison.Ordinal);
        var fallthroughAssignmentIndex = code.IndexOf("r3 = 8;", callIndex, StringComparison.Ordinal);

        Assert.True(callIndex >= 0);
        Assert.True(fallthroughGuardIndex > callIndex);
        Assert.True(localCaseIndex > fallthroughGuardIndex);
        Assert.True(returnIndex > localCaseIndex);
        Assert.True(fallthroughAssignmentIndex > returnIndex, code);
        Assert.Contains("goto loc_800E5934;", code);
    }
}
