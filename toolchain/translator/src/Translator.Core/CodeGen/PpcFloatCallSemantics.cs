using System;

namespace Translator.Core.CodeGen;

public static class PpcFloatCallSemantics
{
    public static bool IsPairedProducerTarget(string target)
    {
        // PPC_PsToScalar returns a scalar value, not a paired-single payload.
        if (target.Equals("PPC_PsToScalar", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        return target.StartsWith("PPC_Ps", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_PsqL", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_LoadPairedSingle", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fres", StringComparison.OrdinalIgnoreCase);
        // Note: PPC_Frsqrte is NOT a paired-single producer - it's a double-precision instruction (opcode 63).
    }

    public static bool IsPairedConsumerTarget(string target)
    {
        // Paired-single consumers expect their operands to already be in paired form.
        // PPC_PsqL consumes an address and returns paired output, while
        // PPC_StorePairedSingle writes raw paired payloads directly.
        if (target.Equals("PPC_PsqL", StringComparison.OrdinalIgnoreCase) ||
            target.Equals("PPC_StorePairedSingle", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        // PPC_PsFromScalar consumes a scalar input and produces paired output.
        if (target.Equals("PPC_PsFromScalar", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        if (target.Equals("PPC_Fres", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        return target.StartsWith("PPC_Ps", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_LoadPairedSingle", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_PsqSt", StringComparison.OrdinalIgnoreCase);
    }

    public static bool IsSinglePrecisionConsumerTarget(string target)
    {
        return target.Equals("PPC_Fadds", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fsubs", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fmuls", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fdivs", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fmadds", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fmsubs", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fnmadds", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fnmsubs", StringComparison.OrdinalIgnoreCase);
    }

    public static bool IsScalarFloatConsumerTarget(string target)
    {
        return target.Equals("PPC_Fctiwz", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fsqrt", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Frsqrte", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fsel", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fmadd", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fmsub", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fnmadd", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fnmsub", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fadds", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fsubs", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fmuls", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fdivs", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fmadds", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fmsubs", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fnmadds", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fnmsubs", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_Fcmp", StringComparison.OrdinalIgnoreCase) ||
               target.Equals("PPC_PsFromScalar", StringComparison.OrdinalIgnoreCase);
    }
}
