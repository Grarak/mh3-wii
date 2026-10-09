using System;
using System.Collections.Generic;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class ResidentAbiCodeGenTests
{
    [Fact]
    public void StateFreeLeafVariantHasOnlyLiveNativeInputsAndNoCpuContext()
    {
        var function = new IrFunction("state_free_store", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrStore(new IrAddress("r3", 4), IrValue.Register("r4"), 4),
                new IrReturn(null)
            })
        });

        var code = new CxxLinearCodeGenerator().Emit(
            0x80063FF0,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("state_free_store", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
            {
                ["r3"] = ValueRepresentation.UInt32,
                ["r4"] = ValueRepresentation.UInt32
            }),
            emitStateFreeLeafVariant: true,
            stateFreeCallSymbols: new Dictionary<uint, string>
            {
                [0x80063FF0] = "state_free_store_native"
            },
            stateFreeEntryVariants: new[]
            {
                new GuestStateFreeCallVariant(
                    0x80063FF0, "state_free_store_native_v0", GuestAbiContractAnalyzer.Analyze(function))
            });

        var variantStart = code.IndexOf(
            "state_free_store_native(uint32_t native_r3, uint32_t native_r4)",
            StringComparison.Ordinal);
        Assert.True(variantStart >= 0, code);
        var markerEnd = code.IndexOf('\n', code.IndexOf("RECOMP_STATE_FREE_ABI", variantStart, StringComparison.Ordinal));
        var variant = code[variantStart..markerEnd];
        Assert.Contains("uint32_t native_r3, uint32_t native_r4", variant, StringComparison.Ordinal);
        Assert.DoesNotContain("CpuContext", variant, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->", variant, StringComparison.Ordinal);
        Assert.DoesNotContain("native_r5", variant, StringComparison.Ordinal);
        Assert.Contains("RECOMP_STATE_FREE_ABI", variant, StringComparison.Ordinal);
        Assert.Contains(
            "extern \"C\" MKW_PPC_ALWAYS_INLINE_BODY void state_free_store_native_v0(",
            code,
            StringComparison.Ordinal);

        var publicStart = code.IndexOf(
            "extern \"C\" void state_free_store(CpuContext* MKW_RESTRICT ctx)",
            StringComparison.Ordinal);
        var publicEnd = code.IndexOf(
            "extern \"C\" MKW_PPC_ALWAYS_INLINE_BODY",
            publicStart,
            StringComparison.Ordinal);
        var publicBody = code[publicStart..publicEnd];
        Assert.Contains("r3", publicBody, StringComparison.Ordinal);
        Assert.DoesNotContain("cached_r3", publicBody, StringComparison.Ordinal);
        Assert.Contains("cached_r3", variant, StringComparison.Ordinal);
    }

    [Fact]
    public void StateFreeLeafElidesDeadLrAssignmentWithNoncanonicalSource()
    {
        const uint address = 0x80063FE0u;
        var function = new IrFunction("state_free_dead_lr", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("lr", IrValue.Register("r0")),
                new IrReturn(null)
            })
        });
        var planned = GuestAbiContractAnalyzer.Analyze(function) with { MayWriteLr = false };

        var code = new CxxLinearCodeGenerator().Emit(
            address,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("state_free_dead_lr", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
            {
                ["r0"] = ValueRepresentation.UInt32,
                ["lr"] = ValueRepresentation.UInt32
            }),
            emitStateFreeLeafVariant: true,
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract> { [address] = planned },
            stateFreeCallSymbols: new Dictionary<uint, string> { [address] = "state_free_dead_lr_native" });

        var start = code.IndexOf("state_free_dead_lr_native(", StringComparison.Ordinal);
        var marker = code.IndexOf("RECOMP_STATE_FREE_ABI", start, StringComparison.Ordinal);
        var variant = code[start..marker];
        Assert.DoesNotContain("ctx->lr", variant, StringComparison.Ordinal);
        Assert.DoesNotContain("CpuContext", variant, StringComparison.Ordinal);
    }

    [Fact]
    public void DirectStateFreeCallUsesLiveArgumentsAndRetainsNormalAbiFallback()
    {
        const uint calleeAddress = 0x80063FF0u;
        var function = new IrFunction("state_free_caller", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall(string.Empty, $"0x{calleeAddress:X8}", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });
        var contract = new GuestAbiContract(
            GprReadBeforeWriteMask: (1u << 3) | (1u << 4),
            GprPossibleWriteMask: 0,
            GprReturnMask: 0,
            FprReadBeforeWriteMask: 0,
            FprPossibleWriteMask: 0,
            FprReturnMask: 0,
            CrReadBeforeWriteMask: 0,
            CrPossibleWriteMask: 0,
            ReadsXerBeforeWrite: false,
            MayWriteXer: false,
            ReadsCtrBeforeWrite: false,
            MayWriteCtr: false,
            ReadsLrBeforeWrite: false,
            MayWriteLr: false,
            BoundaryFlags: GuestCallBoundaryFlags.None,
            DirectCallTargets: Array.Empty<uint>());

        var code = new CxxLinearCodeGenerator().Emit(
            0x80063FE0,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("state_free_caller", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            // The caller itself is emitted as a state-free interface so its own
            // architectural GPRs live in native locals; that is what makes the
            // direct state-free call site pass cached values instead of context.
            emitStateFreeLeafVariant: true,
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract>
            {
                [calleeAddress] = contract,
                [0x80063FE0u] = contract
            },
            stateFreeCallSymbols: new Dictionary<uint, string>
            {
                [calleeAddress] = "state_free_store_native",
                [0x80063FE0u] = "state_free_caller_native"
            });

        Assert.Contains($"IsBaseTranslatedCpuTargetActive<0x{calleeAddress:X8}u>()", code, StringComparison.Ordinal);
        Assert.Contains($"InvokeDirectCpu<0x{calleeAddress:X8}u>(ctx);", code, StringComparison.Ordinal);
        // Inside the caller's own state-free body the call site must consume the
        // native register locals; the retained public CpuContext entry keeps the
        // ordinary ctx-based fallback and is deliberately excluded here.
        var variantStart = code.IndexOf("void state_free_caller_native(", StringComparison.Ordinal);
        Assert.True(variantStart >= 0, "Missing state-free variant for the caller.");
        var variant = code[variantStart..code.IndexOf("RECOMP_STATE_FREE_ABI", variantStart, StringComparison.Ordinal)];
        Assert.Contains("state_free_store_native(cached_r3, cached_r4);", variant, StringComparison.Ordinal);
        Assert.DoesNotContain("state_free_store_native(ctx", variant, StringComparison.Ordinal);
    }

    [Fact]
    public void OversizedStateFreeRegionCannotBypassContextBoundary()
    {
        const uint calleeAddress = 0x80063FF0u;
        var function = new IrFunction("oversized_state_free_caller", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall(string.Empty, $"0x{calleeAddress:X8}", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });
        var contract = new GuestAbiContract(
            GprReadBeforeWriteMask: 0x1Fu,
            GprPossibleWriteMask: 1u << 3,
            GprReturnMask: 1u << 3,
            FprReadBeforeWriteMask: 0,
            FprPossibleWriteMask: 0,
            FprReturnMask: 0,
            CrReadBeforeWriteMask: 0,
            CrPossibleWriteMask: 0,
            ReadsXerBeforeWrite: false,
            MayWriteXer: false,
            ReadsCtrBeforeWrite: false,
            MayWriteCtr: false,
            ReadsLrBeforeWrite: false,
            MayWriteLr: false,
            BoundaryFlags: GuestCallBoundaryFlags.None,
            DirectCallTargets: Array.Empty<uint>());

        var code = new CxxLinearCodeGenerator().Emit(
            0x80063FE0,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("oversized_state_free_caller", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract>
            {
                [calleeAddress] = contract
            },
            stateFreeCallSymbols: new Dictionary<uint, string>
            {
                [calleeAddress] = "oversized_state_free_native"
            });

        Assert.Contains("if (false) {", code, StringComparison.Ordinal);
        Assert.Contains($"InvokeDirectCpu<0x{calleeAddress:X8}u>(ctx);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void ExactCallSiteCanSelectCompactSpecializedResultContract()
    {
        const uint calleeAddress = 0x80063FF0u;
        var function = new IrFunction("state_free_specialized_caller", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall(string.Empty, $"0x{calleeAddress:X8}", Array.Empty<IrValue>()),
                new IrAssign("r7", IrValue.Imm(1)),
                new IrCall(string.Empty, $"0x{calleeAddress:X8}", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });
        var globalContract = new GuestAbiContract(
            (1u << 3) | (1u << 5), (1u << 4) | (1u << 6), 0,
            0, 0, 0, 0, 0,
            false, false, false, false, false, false,
            GuestCallBoundaryFlags.None, Array.Empty<uint>());
        var specializedContract = globalContract with
        {
            GprReadBeforeWriteMask = 1u << 3,
            GprPossibleWriteMask = 1u << 4
        };
        var variant = new GuestStateFreeCallVariant(
            calleeAddress, "state_free_store_native_v0", specializedContract);

        var code = new CxxLinearCodeGenerator().Emit(
            0x80063FE0,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("state_free_specialized_caller", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract> { [calleeAddress] = globalContract },
            stateFreeCallSymbols: new Dictionary<uint, string> { [calleeAddress] = "state_free_store_native" },
            stateFreeCallSiteVariants: new Dictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>
            {
                [new GuestStateFreeCallSiteKey("entry", calleeAddress, 0)] = variant
            });

        Assert.Contains("state_free_store_native_v0(r3)", code, StringComparison.Ordinal);
        Assert.Contains("state_free_store_native(r3, r5)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("state_free_store_native_v0(r5)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void StateFreeDirectCallPassesDefinedFallthroughLrInsteadOfContextLr()
    {
        const uint calleeAddress = 0x80063FF0u;
        var function = new IrFunction("state_free_lr_caller", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("lr", IrValue.Imm(0x80064008u)),
                new IrCall("lr", $"0x{calleeAddress:X8}", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });
        var contract = new GuestAbiContract(
            1u << 3, 0, 0, 0, 0, 0, 0, 0,
            false, false, false, false, true, false,
            GuestCallBoundaryFlags.None, Array.Empty<uint>());

        var code = new CxxLinearCodeGenerator().Emit(
            0x80064000u,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("state_free_lr_caller", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract> { [calleeAddress] = contract },
            stateFreeCallSymbols: new Dictionary<uint, string> { [calleeAddress] = "state_free_lr_native" });

        Assert.Contains("state_free_lr_native(r3, 0x80064008u)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("state_free_lr_native(r3, ctx->lr)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void StateFreeLeafReturnsExplicitGprAndFprStateWithoutCpuContext()
    {
        var function = new IrFunction("state_free_results", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("r3", IrValue.Register("r4")),
                new IrAssign("f1", IrValue.Register("f2")),
                new IrReturn(null)
            })
        });

        var code = new CxxLinearCodeGenerator().Emit(
            0x80063FD0,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("state_free_results", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
            {
                ["r3"] = ValueRepresentation.UInt32,
                ["r4"] = ValueRepresentation.UInt32,
                ["f1"] = ValueRepresentation.Float64,
                ["f2"] = ValueRepresentation.Float64
            }),
            emitStateFreeLeafVariant: true,
            stateFreeCallSymbols: new Dictionary<uint, string>
            {
                [0x80063FD0] = "state_free_results_native"
            });

        var variantStart = code.IndexOf("MkwStateFreeResult2 state_free_results_native(", StringComparison.Ordinal);
        var markerEnd = code.IndexOf('\n', code.IndexOf("RECOMP_STATE_FREE_ABI", variantStart, StringComparison.Ordinal));
        var variant = code[variantStart..markerEnd];
        Assert.Contains("MkwStateFreeResult2", variant, StringComparison.Ordinal);
        Assert.Contains("state_free_results_native(", variant, StringComparison.Ordinal);
        Assert.Contains("PPC_FPR native_f1", variant, StringComparison.Ordinal);
        Assert.Contains("PPC_FPR native_f2", variant, StringComparison.Ordinal);
        Assert.Contains("return { static_cast<uint64_t>(cached_r3), cached_f1.raw };", variant, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->", variant, StringComparison.Ordinal);
    }

    [Fact]
    public void TwoValueSpecializationDoesNotReuseLargerAddressResultStruct()
    {
        const uint address = 0x80063FC0u;
        var function = new IrFunction("state_free_result_collision", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("r3", IrValue.Imm(1)),
                new IrAssign("r4", IrValue.Imm(2)),
                new IrAssign("r5", IrValue.Imm(3)),
                new IrReturn(null)
            })
        });
        var publicContract = GuestAbiContractAnalyzer.Analyze(function);
        var compactContract = publicContract with { GprPossibleWriteMask = (1u << 3) | (1u << 4) };

        var code = new CxxLinearCodeGenerator().Emit(
            address,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("state_free_result_collision", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            emitStateFreeLeafVariant: true,
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract> { [address] = publicContract },
            stateFreeCallSymbols: new Dictionary<uint, string> { [address] = "state_free_result_collision_native" },
            stateFreeEntryVariants: new[]
            {
                new GuestStateFreeCallVariant(address, "state_free_result_collision_native_v0", compactContract)
            });

        Assert.Contains("MkwStateFreeResult_80063FC0 state_free_result_collision_native(", code, StringComparison.Ordinal);
        Assert.Contains("MkwStateFreeResult2 state_free_result_collision_native_v0(", code, StringComparison.Ordinal);
        Assert.DoesNotContain("using MkwStateFreeResult_80063FC0", code, StringComparison.Ordinal);
    }

    [Fact]
    public void DirectStateFreeCallWritesOnlyExplicitResults()
    {
        const uint calleeAddress = 0x80063FD0u;
        var function = new IrFunction("state_free_result_caller", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall(string.Empty, $"0x{calleeAddress:X8}", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });
        var contract = new GuestAbiContract(
            GprReadBeforeWriteMask: 1u << 4,
            GprPossibleWriteMask: 1u << 3,
            GprReturnMask: 1u << 3,
            FprReadBeforeWriteMask: 1u << 2,
            FprPossibleWriteMask: 1u << 1,
            FprReturnMask: 1u << 1,
            CrReadBeforeWriteMask: 0,
            CrPossibleWriteMask: 0,
            ReadsXerBeforeWrite: false,
            MayWriteXer: false,
            ReadsCtrBeforeWrite: false,
            MayWriteCtr: false,
            ReadsLrBeforeWrite: false,
            MayWriteLr: false,
            BoundaryFlags: GuestCallBoundaryFlags.None,
            DirectCallTargets: Array.Empty<uint>());

        var code = new CxxLinearCodeGenerator().Emit(
            0x80063FC0,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("state_free_result_caller", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract> { [calleeAddress] = contract },
            stateFreeCallSymbols: new Dictionary<uint, string> { [calleeAddress] = "state_free_results_native" });

        Assert.Matches(@"const auto state_free_result_[0-9A-F_]+ = state_free_results_native\(", code);
        Assert.Matches(@"r3 = static_cast<uint32_t>\(state_free_result_[0-9A-F_]+\[0\]\);", code);
        Assert.Matches(@"f1\.raw = static_cast<uint64_t>\(state_free_result_[0-9A-F_]+\[1\]\);", code);
        Assert.DoesNotContain("r4 = state_free_result", code, StringComparison.Ordinal);
        Assert.DoesNotContain("f2 = state_free_result", code, StringComparison.Ordinal);
    }

    [Fact]
    public void DirectStateFreeCallExtractsRawBitsFromStructFprResults()
    {
        const uint calleeAddress = 0x80063FBCu;
        var function = new IrFunction("state_free_struct_result_caller", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall(string.Empty, $"0x{calleeAddress:X8}", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });
        var contract = new GuestAbiContract(
            0, 0, 0, 0, (1u << 0) | (1u << 2) | (1u << 3) | (1u << 4), 0,
            0, 0, false, false, false, false, false, false,
            GuestCallBoundaryFlags.None, Array.Empty<uint>());

        var code = new CxxLinearCodeGenerator().Emit(
            0x80063FB8,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("state_free_struct_result_caller", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract> { [calleeAddress] = contract },
            stateFreeCallSymbols: new Dictionary<uint, string> { [calleeAddress] = "state_free_struct_native" });

        Assert.Matches(@"f0\.raw = static_cast<uint64_t>\(state_free_result_[0-9A-F_]+\.f0\.raw\);", code);
        Assert.Matches(@"f4\.raw = static_cast<uint64_t>\(state_free_result_[0-9A-F_]+\.f4\.raw\);", code);
    }

    [Fact]
    public void NonLeafStateFreeVariantCallsNativeCalleeWithoutContextFallback()
    {
        const uint callerAddress = 0x80063FB0u;
        const uint calleeAddress = 0x80063FA0u;
        var callee = new GuestAbiContract(
            1u << 3, 1u << 4, 1u << 4, 0, 0, 0, 0, 0,
            false, false, false, false, false, false,
            GuestCallBoundaryFlags.None, Array.Empty<uint>());
        var caller = new GuestAbiContract(
            1u << 3, (1u << 4) | (1u << 5), 0, 0, 0, 0, 0, 0,
            false, false, false, false, false, false,
            GuestCallBoundaryFlags.None, new[] { calleeAddress });
        var function = new IrFunction("state_free_nonleaf", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall("lr", $"0x{calleeAddress:X8}", Array.Empty<IrValue>()),
                new IrAssign("r5", IrValue.Register("r4")),
                new IrReturn(null)
            })
        });
        var code = new CxxLinearCodeGenerator().Emit(
            callerAddress,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("state_free_nonleaf", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            emitStateFreeLeafVariant: true,
            guestAbiContracts: new Dictionary<uint, GuestAbiContract> { [calleeAddress] = callee },
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract>
            {
                [callerAddress] = caller with { GprPossibleWriteMask = 1u << 5 },
                [calleeAddress] = callee
            },
            stateFreeCallSymbols: new Dictionary<uint, string>
            {
                [callerAddress] = "state_free_nonleaf_native",
                [calleeAddress] = "state_free_callee_native"
            });

        var start = code.IndexOf("state_free_nonleaf_native(", StringComparison.Ordinal);
        var marker = code.IndexOf("RECOMP_STATE_FREE_ABI", start, StringComparison.Ordinal);
        var variant = code[start..marker];
        Assert.Contains("state_free_callee_native(", variant, StringComparison.Ordinal);
        Assert.Contains("return static_cast<uint64_t>(cached_r5);", variant, StringComparison.Ordinal);
        Assert.DoesNotContain("CpuContext", variant, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->", variant, StringComparison.Ordinal);
        Assert.DoesNotContain("InvokeDirectCpu", variant, StringComparison.Ordinal);
        Assert.DoesNotContain("KnownTranslatedCpuCall", variant, StringComparison.Ordinal);
    }

    [Fact]
    public void StateFreeTailCallerForwardsIncomingLrWithoutContextAccess()
    {
        const uint callerAddress = 0x80063FB0u;
        const uint calleeAddress = 0x80063FA0u;
        var callee = new GuestAbiContract(
            1u << 3, 1u << 3, 1u << 3, 0, 0, 0, 0, 0,
            false, false, false, false, true, false,
            GuestCallBoundaryFlags.None, Array.Empty<uint>());
        var caller = new GuestAbiContract(
            1u << 3, 1u << 3, 1u << 3, 0, 0, 0, 0, 0,
            false, false, false, false, false, false,
            GuestCallBoundaryFlags.None, new[] { calleeAddress });
        var function = new IrFunction("state_free_tail_caller", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall(string.Empty, $"0x{calleeAddress:X8}", Array.Empty<IrValue>()),
                new IrReturn(null)
            })
        });

        var code = new CxxLinearCodeGenerator().Emit(
            callerAddress,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("state_free_tail_caller", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            emitStateFreeLeafVariant: true,
            guestAbiContracts: new Dictionary<uint, GuestAbiContract> { [calleeAddress] = callee },
            stateFreeAbiContracts: new Dictionary<uint, GuestAbiContract>
            {
                [callerAddress] = caller,
                [calleeAddress] = callee
            },
            stateFreeCallSymbols: new Dictionary<uint, string>
            {
                [callerAddress] = "state_free_tail_caller_native",
                [calleeAddress] = "state_free_tail_callee_native"
            });

        var start = code.IndexOf("state_free_tail_caller_native(", StringComparison.Ordinal);
        var marker = code.IndexOf("RECOMP_STATE_FREE_ABI", start, StringComparison.Ordinal);
        var variant = code[start..marker];
        Assert.Contains("uint32_t native_lr", variant, StringComparison.Ordinal);
        Assert.Contains("state_free_tail_callee_native(cached_r3, native_lr)", variant, StringComparison.Ordinal);
        Assert.DoesNotContain("ctx->", variant, StringComparison.Ordinal);
    }

    [Fact]
    public void PublicLeafDoesNotElideNonvolatileGprStackRoundTrip()
    {
        var function = new IrFunction("public_leaf_gpr_save", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrStore(new IrAddress("r1", 12), IrValue.Register("r31"), 4),
                new IrAssign("r31", IrValue.Imm(7)),
                new IrLoad("r31", new IrAddress("r1", 12), 4),
                new IrReturn(null)
            })
        });

        var code = new CxxLinearCodeGenerator().Emit(
            0x80064008,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("public_leaf_gpr_save", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()),
            enableLeafAbiSpillElision: true);

        Assert.Contains("FlatWriteRam32", code, StringComparison.Ordinal);
        Assert.Contains("FlatRead32", code, StringComparison.Ordinal);
        Assert.Contains("r31 = 7", code, StringComparison.Ordinal);
        Assert.Contains("r31 = MemoryInline::FlatRead32", code, StringComparison.Ordinal);
    }

    [Fact]
    public void GprOnlyLeafCacheDoesNotImplicitlyElideGuestAbiSpills()
    {
        var function = new IrFunction("cached_leaf_gpr_save", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrStore(new IrAddress("r1", 12), IrValue.Register("r31"), 4),
                new IrAssign("r31", IrValue.Imm(7)),
                new IrLoad("r31", new IrAddress("r1", 12), 4),
                new IrReturn(null)
            })
        });

        var code = new CxxLinearCodeGenerator().Emit(
            0x80064010,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("cached_leaf_gpr_save", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()));

        Assert.Contains("FlatWriteRam32", code, StringComparison.Ordinal);
        Assert.Contains("FlatRead32", code, StringComparison.Ordinal);
    }

    [Fact]
    public void StackFastPathFollowsFramePointerCopiedIntoArchitecturalGpr()
    {
        var function = new IrFunction("derived_stack_gpr", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrBinary("r1", IrValue.Register("r1"), IrValue.Imm(64), "sub"),
                new IrBinary("r11", IrValue.Register("r1"), IrValue.Imm(64), "add"),
                new IrStore(new IrAddress("r11", -28), IrValue.Register("r25"), 4),
                new IrLoad("r25", new IrAddress("r11", -28), 4),
                new IrReturn(null)
            })
        });

        var code = new CxxLinearCodeGenerator().Emit(
            0x80064014,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("derived_stack_gpr", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()));

        // A stack slot store skips the MMIO policy check (FlatWriteRam) instead of the general guarded
        // write (FlatWrite); reads have no such distinction, both spell as FlatRead.
        Assert.Contains("MemoryInline::FlatWriteRam32", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatRead32", code, StringComparison.Ordinal);
        Assert.DoesNotContain("MemoryInline::FlatWrite32", code, StringComparison.Ordinal);
    }

    [Fact]
    public void StackFastPathFollowsMultipleSsaDerivedAddressRegisters()
    {
        var function = new IrFunction("transitive_stack_gpr", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("r11", IrValue.Register("r1")),
                new IrBinary("r12", IrValue.Register("r11"), IrValue.Imm(32), "add"),
                new IrStore(new IrAddress("r12", -8), IrValue.Register("r31"), 4),
                new IrLoad("r31", new IrAddress("r12", -8), 4),
                new IrReturn(null)
            })
        });

        var code = new CxxLinearCodeGenerator().Emit(
            0x80064018,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("transitive_stack_gpr", ValueRepresentation.Void),
            new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>()));

        // See StackFastPathFollowsFramePointerCopiedIntoArchitecturalGpr: only
        // the store side still distinguishes the stack fast path in the emitted
        // helper name.
        Assert.Contains("MemoryInline::FlatWriteRam32", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatRead32", code, StringComparison.Ordinal);
        Assert.DoesNotContain("MemoryInline::FlatWrite32", code, StringComparison.Ordinal);
    }

    [Fact]
    public void GuestAbiContractModelsInlineGprThunkRegisterEffects()
    {
        var function = new IrFunction("thunk_contract", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall(string.Empty, "0x80021580", Array.Empty<IrValue>()), // save r19-r31
                new IrCall(string.Empty, "0x800215CC", Array.Empty<IrValue>()), // rest r19-r31
                new IrReturn(null)
            })
        });

        var contract = GuestAbiContractAnalyzer.Analyze(function);
        const uint expected = 0xFFF80000u;
        Assert.Equal(expected, contract.GprReadBeforeWriteMask & expected);
        Assert.Equal(expected, contract.GprPossibleWriteMask & expected);
        Assert.Empty(contract.DirectCallTargets);
    }

}
