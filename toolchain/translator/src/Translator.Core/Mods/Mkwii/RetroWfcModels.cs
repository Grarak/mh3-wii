namespace Translator.Core.Mods.Mkwii;

/// <summary>
/// A target synthesized while parsing the active WWFC payload descriptor.
/// </summary>
public sealed record RetroWfcSemanticActionTarget(
    string ActionId,
    string Kind,
    uint GuestAddress,
    uint ModuleOffset,
    string? Symbol,
    string? Source);

/// <summary>
/// A constructor or initialization callback discovered in the active payload image.
/// </summary>
public sealed record RetroWfcHelperInitCallback(
    string Kind,
    uint TargetAddress,
    uint ModuleOffset,
    string? Symbol);
