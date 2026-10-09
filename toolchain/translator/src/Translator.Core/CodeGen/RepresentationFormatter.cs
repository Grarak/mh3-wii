using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public static class RepresentationFormatter
{
    public static string ToCxx(ValueRepresentation representation)
    {
        if (representation == ValueRepresentation.Void) return "void";
        if (representation == ValueRepresentation.Int32) return "int32_t";
        if (representation == ValueRepresentation.UInt32) return "uint32_t";
        if (representation == ValueRepresentation.Int64) return "int64_t";
        if (representation == ValueRepresentation.UInt64) return "uint64_t";
        if (representation == ValueRepresentation.Float32) return "float";
        if (representation == ValueRepresentation.Float64) return "double";
        return "uint32_t";
    }

    public static string DefaultValue(ValueRepresentation representation)
    {
        if (representation == ValueRepresentation.Void) return string.Empty;
        if (representation == ValueRepresentation.Float32) return "0.0f";
        if (representation == ValueRepresentation.Float64) return "0.0";
        return "0";
    }
}

