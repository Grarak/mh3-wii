using System.Buffers.Binary;
using Translator.Core.IO;

namespace Translator.Core.Parsing.Kamek;

public sealed class KamekChunk
{
    public const uint Magic0 = 0x4B616D65;
    /// <summary>
    /// Kamek v2 is still emitted by current Retro Rewind releases. Its command
    /// stream matches v3, but combined v2 files carry each chunk length only in
    /// the outer size table.
    /// </summary>
    public const uint Magic1V2 = 0x6B000002;
    public const uint Magic1 = 0x6B000003;
    public const int HeaderSize = 0x20;

    public KamekChunk(
        int index,
        long fileOffset,
        uint bssSize,
        uint codeSize,
        uint ctorStart,
        uint ctorEnd,
        uint chunkSize,
        byte[] codeBlob,
        IReadOnlyList<KamekCommand> commands)
    {
        Index = index;
        FileOffset = fileOffset;
        BssSize = bssSize;
        CodeSize = codeSize;
        CtorStart = ctorStart;
        CtorEnd = ctorEnd;
        ChunkSize = chunkSize;
        CodeBlob = codeBlob;
        Commands = commands;
    }

    public int Index { get; }
    public long FileOffset { get; }
    public uint BssSize { get; }
    public uint CodeSize { get; }
    public uint CtorStart { get; }
    public uint CtorEnd { get; }
    public uint ChunkSize { get; }
    public byte[] CodeBlob { get; }
    public IReadOnlyList<KamekCommand> Commands { get; }

    public int RelativeCommandCount => Commands.Count(c => c.AddressIsRelative);
    public int AbsoluteCommandCount => Commands.Count(c => c.AddressIsAbsolute);

    public IReadOnlyDictionary<KamekCommandId, int> CommandCounts =>
        Commands
            .GroupBy(c => c.Id)
            .OrderBy(g => (byte)g.Key)
            .ToDictionary(g => g.Key, g => g.Count());

    public static bool HasMagic(ReadOnlySpan<byte> data, int offset)
    {
        if (offset < 0 || offset + 8 > data.Length)
        {
            return false;
        }

        return BinaryPrimitives.ReadUInt32BigEndian(data.Slice(offset, 4)) == Magic0 &&
            IsSupportedFormatVersion(BinaryPrimitives.ReadUInt32BigEndian(data.Slice(offset + 4, 4)));
    }

    public static KamekChunk Parse(byte[] data, int offset, int expectedSize, int index)
    {
        if (offset < 0 || offset + HeaderSize > data.Length)
        {
            throw new InvalidDataException($"Kamek chunk {index} header at 0x{offset:X} is outside file.");
        }

        using var stream = new MemoryStream(data, writable: false);
        using var reader = new BigEndianBinaryReader(stream);
        reader.Seek(offset, SeekOrigin.Begin);

        var magic0 = reader.ReadUInt32();
        var magic1 = reader.ReadUInt32();
        if (magic0 != Magic0 || !IsSupportedFormatVersion(magic1))
        {
            throw new InvalidDataException($"Kamek chunk {index} at 0x{offset:X} has invalid magic 0x{magic0:X8}/0x{magic1:X8}.");
        }

        var bssSize = reader.ReadUInt32();
        var codeSize = reader.ReadUInt32();
        var ctorStart = reader.ReadUInt32();
        var ctorEnd = reader.ReadUInt32();
        var declaredChunkSize = reader.ReadUInt32();
        _ = reader.ReadUInt32(); // reserved

        // v3 writes each chunk's length in its header. v2 leaves that field at
        // zero and relies on the combined-file table (or the raw file length).
        var chunkSize = magic1 == Magic1V2
            ? checked((uint)(expectedSize != 0 ? expectedSize : data.Length - offset))
            : declaredChunkSize;

        if (chunkSize < HeaderSize + codeSize)
        {
            throw new InvalidDataException($"Kamek chunk {index} chunkSize 0x{chunkSize:X} is smaller than header+code.");
        }

        if (expectedSize != 0 && chunkSize != expectedSize)
        {
            throw new InvalidDataException($"Kamek chunk {index} chunkSize 0x{chunkSize:X} does not match combined size 0x{expectedSize:X}.");
        }

        if (offset + chunkSize > data.Length)
        {
            throw new InvalidDataException($"Kamek chunk {index} extends past file end.");
        }

        if (ctorStart > codeSize || ctorEnd > codeSize || ctorStart > ctorEnd || ((ctorEnd - ctorStart) % 4) != 0)
        {
            throw new InvalidDataException($"Kamek chunk {index} has invalid ctor range 0x{ctorStart:X}-0x{ctorEnd:X} for codeSize 0x{codeSize:X}.");
        }

        var codeBlob = reader.ReadBytes(checked((int)codeSize));
        var commandEnd = offset + checked((int)chunkSize);
        var commands = new List<KamekCommand>();

        while (reader.Position < commandEnd)
        {
            var commandOffset = reader.Position;
            var commandWord = reader.ReadUInt32();
            var rawId = (byte)(commandWord >> 24);
            if (!Enum.IsDefined(typeof(KamekCommandId), rawId))
            {
                throw new InvalidDataException($"Unknown Kamek command id {rawId} at file offset 0x{commandOffset:X}.");
            }

            var id = (KamekCommandId)rawId;
            var addressEnc = commandWord & 0x00FFFFFFu;
            bool addressIsAbsolute;
            uint address;
            if (addressEnc == 0x00FFFFFEu)
            {
                addressIsAbsolute = true;
                address = reader.ReadUInt32();
            }
            else
            {
                addressIsAbsolute = false;
                address = addressEnc;
            }

            var argCount = ArgumentCount(id);
            var args = new uint[argCount];
            for (var i = 0; i < argCount; i++)
            {
                args[i] = reader.ReadUInt32();
            }

            commands.Add(new KamekCommand(commandOffset, commandWord, id, addressIsAbsolute, address, args));
        }

        if (reader.Position != commandEnd)
        {
            throw new InvalidDataException($"Kamek chunk {index} command stream ended at 0x{reader.Position:X}, expected 0x{commandEnd:X}.");
        }

        return new KamekChunk(index, offset, bssSize, codeSize, ctorStart, ctorEnd, chunkSize, codeBlob, commands);
    }

    public static int ArgumentCount(KamekCommandId id) => id switch
    {
        KamekCommandId.Addr32 => 1,
        KamekCommandId.Addr16Lo => 1,
        KamekCommandId.Addr16Hi => 1,
        KamekCommandId.Addr16Ha => 1,
        KamekCommandId.Rel24 => 1,
        KamekCommandId.Write32 => 1,
        KamekCommandId.Write16 => 1,
        KamekCommandId.Write8 => 1,
        KamekCommandId.CondWritePointer => 2,
        KamekCommandId.CondWrite32 => 2,
        KamekCommandId.CondWrite16 => 2,
        KamekCommandId.CondWrite8 => 2,
        KamekCommandId.Branch => 1,
        KamekCommandId.BranchLink => 1,
        _ => throw new ArgumentOutOfRangeException(nameof(id), id, "Unsupported Kamek command id")
    };

    private static bool IsSupportedFormatVersion(uint magic1) =>
        magic1 is Magic1V2 or Magic1;
}
