using Translator.Core.Parsing.Kamek;

namespace Translator.Core.Mods.Mkwii;

public static class RetroWfcBootstrapSuppressor
{
    public static KamekChunk Suppress(KamekChunk chunk, uint hookAddress)
    {
        ArgumentNullException.ThrowIfNull(chunk);

        var matching = chunk.Commands
            .Where(command => command.AddressIsAbsolute && command.Address == hookAddress)
            .ToArray();
        if (matching.Length != 1 ||
            matching[0].Id != KamekCommandId.Branch ||
            matching[0].Arguments.Count != 1)
        {
            throw new InvalidDataException(
                $"Expected exactly one absolute Branch command at the configured Retro WFC legacy bootstrap hook " +
                $"0x{hookAddress:X8}, but found {matching.Length} matching command(s).");
        }

        var commands = chunk.Commands
            .Where(command => !ReferenceEquals(command, matching[0]))
            .ToArray();
        return new KamekChunk(
            chunk.Index,
            chunk.FileOffset,
            chunk.BssSize,
            chunk.CodeSize,
            chunk.CtorStart,
            chunk.CtorEnd,
            chunk.ChunkSize,
            chunk.CodeBlob,
            commands);
    }
}
