using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Core.Analysis.Representation;

public sealed record FunctionAbiClassification(
    string Name,
    ValueRepresentation ReturnRepresentation);

public sealed class FunctionAbiClassifier
{
    private static readonly string[] AbiIntegerReturnOrder = { "r3", "r4" };
    private static readonly string[] AbiFloatReturnOrder = { "f1" };

    public FunctionAbiClassification Classify(
        string functionName,
        IrFunction function,
        RepresentationEnvironment representations) =>
        new(functionName, InferReturnRepresentation(function, representations));

    private static ValueRepresentation InferReturnRepresentation(IrFunction function, RepresentationEnvironment types)
    {
        var explicitReturnRepresentations = function.Blocks
            .SelectMany(b => b.Instructions)
            .OfType<IrReturn>()
            .Where(r => r.Value != null)
            .Select(r => InferReturnRepresentationFromValue(r.Value!, types))
            .Where(t => t != null)
            .Cast<ValueRepresentation>()
            .ToList();

        if (explicitReturnRepresentations.Count > 0)
        {
            return MergeReturnRepresentations(explicitReturnRepresentations);
        }

        var cfg = IrCfg.Build(function);
        var inferredReturnRepresentations = new List<ValueRepresentation>();
        foreach (var block in function.Blocks)
        {
            if (block.Instructions.LastOrDefault() is not IrReturn)
            {
                continue;
            }

            inferredReturnRepresentations.AddRange(CollectAbiReturnRepresentationsNearExit(block.Label, cfg, types, new HashSet<string>(StringComparer.OrdinalIgnoreCase)));
        }

        return inferredReturnRepresentations.Count == 0
            ? ValueRepresentation.Void
            : MergeReturnRepresentations(inferredReturnRepresentations);
    }

    private static ValueRepresentation? InferReturnRepresentationFromValue(IrValue val, RepresentationEnvironment types)
    {
        if (val.Kind == "register" && val.RegisterName != null)
        {
            return types.Get(Base(val.RegisterName));
        }

        return ValueRepresentation.Int32;
    }

    private static IEnumerable<ValueRepresentation> CollectAbiReturnRepresentationsNearExit(
        string label,
        IrCfg cfg,
        RepresentationEnvironment types,
        HashSet<string> visited)
    {
        if (!visited.Add(label) || !cfg.Blocks.TryGetValue(label, out var block))
        {
            yield break;
        }

        var localType = FindRecentAbiReturnRepresentation(block, types);
        if (localType != null)
        {
            yield return localType;
            yield break;
        }

        foreach (var pred in cfg.Predecessors(label))
        {
            foreach (var predType in CollectAbiReturnRepresentationsNearExit(pred, cfg, types, visited))
            {
                yield return predType;
            }
        }
    }

    private static ValueRepresentation? FindRecentAbiReturnRepresentation(IrBasicBlock block, RepresentationEnvironment types)
    {
        var limit = block.Instructions.Count;
        var terminator = limit > 0 ? block.Instructions[limit - 1] : null;
        var isDirectReturnBlock = terminator is IrReturn;
        if (isDirectReturnBlock)
        {
            limit--;
        }

        for (var i = limit - 1; i >= 0; i--)
        {
            if (!TryGetDefinedRegister(block.Instructions[i], out var definedRegister))
            {
                continue;
            }

            var baseName = Base(definedRegister);
            if (AbiFloatReturnOrder.Contains(baseName, StringComparer.OrdinalIgnoreCase))
            {
                if (!CanFloatDefineImplicitReturn(block, i, limit, terminator))
                {
                    continue;
                }

                return types.Get(baseName);
            }

            if (AbiIntegerReturnOrder.Contains(baseName, StringComparer.OrdinalIgnoreCase))
            {
                return types.Get(baseName);
            }
        }

        return null;
    }

    private static bool CanFloatDefineImplicitReturn(IrBasicBlock block, int candidateIndex, int limit, IrInstruction? terminator)
    {
        if (terminator is not (IrReturn or IrJump))
        {
            return false;
        }

        var epilogueLimit = terminator is IrJump ? Math.Max(candidateIndex + 1, limit - 1) : limit;
        for (var i = candidateIndex + 1; i < epilogueLimit; i++)
        {
            if (!IsDirectReturnFloatEpilogueInstruction(block.Instructions[i]))
            {
                return false;
            }
        }

        return true;
    }

    private static bool IsDirectReturnFloatEpilogueInstruction(IrInstruction instruction)
    {
        return instruction switch
        {
            IrAssign => true,
            IrBinary => true,
            IrLoad load when string.Equals(Base(load.Address.Base), "r1", StringComparison.OrdinalIgnoreCase) => true,
            IrComment => true,
            IrTracePpc => true,
            _ => false
        };
    }

    private static bool TryGetDefinedRegister(IrInstruction ins, out string register)
    {
        register = string.Empty;
        switch (ins)
        {
            case IrAssign a when !string.IsNullOrWhiteSpace(a.Destination):
                register = a.Destination;
                return true;
            case IrBinary b when !string.IsNullOrWhiteSpace(b.Destination):
                register = b.Destination;
                return true;
            case IrLoad l when !string.IsNullOrWhiteSpace(l.Destination):
                register = l.Destination;
                return true;
            case IrCall c when !string.IsNullOrWhiteSpace(c.Destination):
                register = c.Destination;
                return true;
            case IrIndirectCall ic when !string.IsNullOrWhiteSpace(ic.Destination):
                register = ic.Destination;
                return true;
            case IrPhi p when !string.IsNullOrWhiteSpace(p.Destination):
                register = p.Destination;
                return true;
            default:
                return false;
        }
    }

    private static ValueRepresentation MergeReturnRepresentations(IReadOnlyList<ValueRepresentation> returnTypes)
    {
        var distinct = returnTypes.Distinct().ToList();
        if (distinct.Count == 1)
        {
            return distinct[0];
        }

        var floatCandidate = distinct.OfType<ValueRepresentation>().FirstOrDefault(t => t.IsFloat);
        var nonFloatCandidate = distinct.OfType<ValueRepresentation>().FirstOrDefault(t => !t.IsFloat);

        if (floatCandidate != null && nonFloatCandidate == null)
        {
            return floatCandidate;
        }

        if (nonFloatCandidate != null && floatCandidate == null)
        {
            return nonFloatCandidate;
        }

        return ValueRepresentation.Int32;
    }

    private static string Base(string name) => RegisterNameUtils.StripNumericSuffix(name);
}
