using System;
using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class ResolvedMemoryLeafCacheCodeGenTests
{
    [Fact]
    public void AddressBasesUsedOnlyByResolvedOperationsStayCached()
    {
        var function = new IrFunction(
            "resolved_memory_register_cache",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrResolveGuestMemoryRange(
                        "range", IrValue.Register("r13"), 0, 64,
                        NeedsReadAccess: true, NeedsWriteAccess: false),
                    new IrResolvedLoad(
                        "r3", "range", new IrAddress("r2", 4), 4, 4),
                    new IrReturn(IrValue.Register("r3"))
                })
            });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r2"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32,
            ["r13"] = ValueRepresentation.UInt32
        });

        // Register residency owns every primary body and achieves the property
        // under test (an address base used only by resolved operations promoted
        // once to a local) directly at emission time, under its own plain-named
        // locals.
        var code = new CxxLinearCodeGenerator().Emit(
            0x80006202,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("resolved_memory_register_cache", ValueRepresentation.UInt32),
            types);

        Assert.Contains("uint32_t r2 = ctx->gpr[2];", code, StringComparison.Ordinal);
        Assert.Contains("uint32_t r13 = ctx->gpr[13];", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::ResolveRangeHost(r13", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::ReadResolved32(range, 4u, (r2 + 4))", code, StringComparison.Ordinal);
    }
}
