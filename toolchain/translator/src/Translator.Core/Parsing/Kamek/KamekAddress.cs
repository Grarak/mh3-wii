namespace Translator.Core.Parsing.Kamek;

/// <summary>
/// Kamek command operands are either an absolute guest address or an offset into
/// the loaded module. The 0x80000000 split is the whole rule, and every planner
/// that walks a command stream needs it.
/// </summary>
public static class KamekAddress
{
    public static uint Resolve(uint raw, uint moduleGuestBase) =>
        raw >= 0x80000000u ? raw : checked(moduleGuestBase + raw);
}
