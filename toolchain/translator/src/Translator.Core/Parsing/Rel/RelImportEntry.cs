namespace Translator.Core.Parsing.Rel;

public sealed class RelImportEntry
{
    public RelImportEntry(uint moduleId, uint relocationOffset)
    {
        ModuleId = moduleId;
        RelocationOffset = relocationOffset;
    }

    public uint ModuleId { get; }
    public uint RelocationOffset { get; }
}
