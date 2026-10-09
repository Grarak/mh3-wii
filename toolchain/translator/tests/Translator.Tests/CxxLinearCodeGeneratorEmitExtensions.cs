using System.Collections.Generic;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Representation;

namespace Translator.Tests;

/// <summary>
/// Test-only wrapper around <see cref="CxxLinearCodeGenerator.EmitWithFacts"/> that returns just the
/// emitted C++ text, so codegen tests don't have to thread an unused facts object through every call.
/// </summary>
internal static class CxxLinearCodeGeneratorEmitExtensions
{
    public static string Emit(
        this CxxLinearCodeGenerator generator,
        uint entryPoint,
        SsaResult ssa,
        FunctionAbiClassification signature,
        RepresentationEnvironment types,
        bool emitModRegistration = false,
        uint modRegistrationPriority = 100,
        ulong modRegistrationModuleId = 1,
        IReadOnlySet<uint>? nonReturningCallTargets = null,
        IReadOnlySet<uint>? lrContinuationCallTargets = null,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts = null,
        bool emitStateFreeLeafVariant = false,
        IReadOnlyDictionary<uint, GuestAbiContract>? stateFreeAbiContracts = null,
        IReadOnlyDictionary<uint, string>? stateFreeCallSymbols = null,
        IReadOnlyDictionary<GuestStateFreeCallSiteKey, GuestStateFreeCallVariant>? stateFreeCallSiteVariants = null,
        IReadOnlyList<GuestStateFreeCallVariant>? stateFreeEntryVariants = null,
        uint? moduleLinkBase = null,
        uint? moduleGuestBase = null,
        uint moduleLinkedCodeSize = 0,
        IReadOnlyDictionary<string, uint>? gqrEntryConstants = null,
        IReadOnlyDictionary<uint, byte>? gqrCalleeWriteMasks = null,
        bool gqrConstantsRequireRuntimeGuard = false,
        bool enableLeafAbiSpillElision = false,
        IReadOnlySet<uint>? modOverridableCallTargets = null,
        bool enableGpuFifoBurstCoalescing = true) =>
        generator.EmitWithFacts(
            entryPoint: entryPoint,
            ssa: ssa,
            signature: signature,
            types: types,
            emitModRegistration: emitModRegistration,
            modRegistrationPriority: modRegistrationPriority,
            modRegistrationModuleId: modRegistrationModuleId,
            nonReturningCallTargets: nonReturningCallTargets,
            lrContinuationCallTargets: lrContinuationCallTargets,
            guestAbiContracts: guestAbiContracts,
            emitStateFreeLeafVariant: emitStateFreeLeafVariant,
            stateFreeAbiContracts: stateFreeAbiContracts,
            stateFreeCallSymbols: stateFreeCallSymbols,
            stateFreeCallSiteVariants: stateFreeCallSiteVariants,
            stateFreeEntryVariants: stateFreeEntryVariants,
            moduleLinkBase: moduleLinkBase,
            moduleGuestBase: moduleGuestBase,
            moduleLinkedCodeSize: moduleLinkedCodeSize,
            gqrEntryConstants: gqrEntryConstants,
            gqrCalleeWriteMasks: gqrCalleeWriteMasks,
            gqrConstantsRequireRuntimeGuard: gqrConstantsRequireRuntimeGuard,
            enableLeafAbiSpillElision: enableLeafAbiSpillElision,
            modOverridableCallTargets: modOverridableCallTargets,
            enableGpuFifoBurstCoalescing: enableGpuFifoBurstCoalescing).Code;
}
