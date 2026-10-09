namespace Translator.Core.Disassembly;

/// <summary>
/// Allocation-free view of the fixed fields shared by the raw PowerPC instruction scanners.
/// Not a second semantic decoder: callers still decide which fields matter for a given opcode.
/// </summary>
internal readonly struct PpcWordFields
{
    private readonly uint Word;

    public PpcWordFields(uint word) => Word = word;

    /// <summary>
    /// The big-endian instruction word at <paramref name="offset"/>. Three raw
    /// scanners carried a byte-identical private copy of this one-liner.
    /// </summary>
    public static uint ReadBigEndianWord(byte[] data, int offset) =>
        System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(offset, 4));

    public int PrimaryOpcode => (int)((Word >> 26) & 0x3Fu);

    /// <summary>Bits 21-25, named RS/RT depending on the instruction form.</summary>
    public int GprField0 => (int)((Word >> 21) & 0x1Fu);

    /// <summary>Bits 16-20, normally RA.</summary>
    public int GprField1 => (int)((Word >> 16) & 0x1Fu);

    /// <summary>Bits 11-15, normally RB.</summary>
    public int GprField2 => (int)((Word >> 11) & 0x1Fu);

    public int ExtendedOpcode => (int)((Word >> 1) & 0x3FFu);

    /// <summary>
    /// The split SPR field used by mfspr/mtspr. The low five SPR bits are in
    /// bits 16-20 and the high five bits are in bits 11-15.
    /// </summary>
    public int Spr => (int)(((Word >> 16) & 0x1Fu) | ((Word >> 6) & 0x3E0u));

    public short SignedImmediate16 => unchecked((short)(Word & 0xFFFFu));

    public uint UnsignedImmediate16 => Word & 0xFFFFu;

}

/// <summary>
/// Small raw-word recognizers shared by module and patch scanners, ordered by the names in
/// each instruction form rather than the decoder's alias-oriented operand list.
/// </summary>
internal static class PpcInstructionPatterns
{
    public static bool TryGetLis(uint word, out int destination, out uint highImmediate)
    {
        var fields = new PpcWordFields(word);
        if (fields.PrimaryOpcode == 15 && fields.GprField1 == 0)
        {
            destination = fields.GprField0;
            highImmediate = fields.UnsignedImmediate16;
            return true;
        }

        destination = 0;
        highImmediate = 0;
        return false;
    }

    public static bool TryGetAddi(uint word, out int destination, out int source, out int immediate)
    {
        var fields = new PpcWordFields(word);
        if (fields.PrimaryOpcode == 14)
        {
            destination = fields.GprField0;
            source = fields.GprField1;
            immediate = fields.SignedImmediate16;
            return true;
        }

        destination = 0;
        source = 0;
        immediate = 0;
        return false;
    }

    public static bool TryGetOri(uint word, out int source, out int destination, out uint immediate) =>
        TryGetLogicalImmediate(word, 24, out source, out destination, out immediate);

    public static bool TryGetOris(uint word, out int source, out int destination, out uint immediate) =>
        TryGetLogicalImmediate(word, 25, out source, out destination, out immediate);

    private static bool TryGetLogicalImmediate(
        uint word,
        int opcode,
        out int source,
        out int destination,
        out uint immediate)
    {
        var fields = new PpcWordFields(word);
        if (fields.PrimaryOpcode == opcode)
        {
            source = fields.GprField0;
            destination = fields.GprField1;
            immediate = fields.UnsignedImmediate16;
            return true;
        }

        source = 0;
        destination = 0;
        immediate = 0;
        return false;
    }

    public static bool TryGetOr(uint word, out int source, out int destination, out int otherSource)
    {
        var fields = new PpcWordFields(word);
        if (fields.PrimaryOpcode == 31 && fields.ExtendedOpcode == 444)
        {
            source = fields.GprField0;
            destination = fields.GprField1;
            otherSource = fields.GprField2;
            return true;
        }

        source = 0;
        destination = 0;
        otherSource = 0;
        return false;
    }

    public static bool TryGetRlwinm(
        uint word,
        out int source,
        out int destination,
        out int shift,
        out int maskBegin,
        out int maskEnd)
    {
        var fields = new PpcWordFields(word);
        if (fields.PrimaryOpcode == 21)
        {
            source = fields.GprField0;
            destination = fields.GprField1;
            shift = fields.GprField2;
            maskBegin = (int)((word >> 6) & 0x1Fu);
            maskEnd = (int)((word >> 1) & 0x1Fu);
            return true;
        }

        source = 0;
        destination = 0;
        shift = 0;
        maskBegin = 0;
        maskEnd = 0;
        return false;
    }

    public static bool TryGetMtspr(uint word, int expectedSpr, out int source)
    {
        var fields = new PpcWordFields(word);
        if (fields.PrimaryOpcode == 31 &&
            fields.ExtendedOpcode == 467 &&
            fields.Spr == expectedSpr)
        {
            source = fields.GprField0;
            return true;
        }

        source = 0;
        return false;
    }
}

/// <summary>
/// Branch recognizers for raw image scanners. Separate linked/unlinked forms preserve the
/// b/bl distinction and avoid accepting absolute or indirect branches.
/// </summary>
internal static class PpcControlFlow
{
    private const uint RelativeBranchMask = 0xFC000003u;
    private const uint RelativeBranch = 0x48000000u;
    private const uint RelativeBranchLink = 0x48000001u;

    public static bool IsRelativeUnlinkedBranch(uint word) =>
        (word & RelativeBranchMask) == RelativeBranch;

    public static bool IsRelativeLinkedBranch(uint word) =>
        (word & RelativeBranchMask) == RelativeBranchLink;

    public static bool TryDecodeRelativeBranchTarget(uint address, uint word, out uint target)
    {
        if (!IsRelativeUnlinkedBranch(word))
        {
            target = 0;
            return false;
        }

        target = DecodeRelative24Target(address, word);
        return true;
    }

    public static bool TryDecodeRelativeBranchLinkTarget(uint address, uint word, out uint target)
    {
        if (!IsRelativeLinkedBranch(word))
        {
            target = 0;
            return false;
        }

        target = DecodeRelative24Target(address, word);
        return true;
    }

    public static bool TryDecodeConditionalRelativeBranchTarget(uint address, uint word, out uint target)
    {
        var fields = new PpcWordFields(word);
        if (fields.PrimaryOpcode != 16 || (word & 0x2u) != 0)
        {
            target = 0;
            return false;
        }

        var offset = word & 0xFFFCu;
        if ((offset & 0x8000u) != 0)
        {
            offset |= 0xFFFF0000u;
        }

        target = unchecked((uint)((int)address + (int)offset));
        return true;
    }

    public static bool MayChangeControlFlow(uint word) =>
        new PpcWordFields(word).PrimaryOpcode is 16 or 18 or 19;

    public static bool IsReturn(uint word) => word == 0x4E800020u;

    public static bool IsBctr(uint word) => word == 0x4E800420u;

    private static uint DecodeRelative24Target(uint address, uint word)
    {
        var offset = word & 0x03FFFFFCu;
        if ((offset & 0x02000000u) != 0)
        {
            offset |= 0xFC000000u;
        }

        return unchecked((uint)((int)address + (int)offset));
    }
}

/// <summary>
/// One conservative "which GPR can this raw word clobber" policy, shared by every raw scanner tracking
/// a register across straight-line code. <c>ori</c>/<c>rlwinm</c>/<c>andi.</c> write RA (bits 16-20),
/// not RS; every arm over-approximates on purpose, since "no" wrongly emits a patch at the wrong address.
/// </summary>
internal static class PpcRegisterEffects
{
    /// <summary>
    /// True when this word can write <paramref name="register"/>. FPR/CR/memory destinations
    /// answer false for every GPR, except update forms which also write back to RA.
    /// </summary>
    public static bool MayWriteGpr(uint word, int register)
    {
        var fields = new PpcWordFields(word);
        var rt = fields.GprField0;
        var ra = fields.GprField1;
        return fields.PrimaryOpcode switch
        {
            // D-form arithmetic and plain loads: destination is RT.
            7 or 8 or 12 or 13 or 14 or 15 or 32 or 34 or 40 or 42 => rt == register,
            // Load with update: RT and the RA base are both written.
            33 or 35 or 41 or 43 => rt == register || ra == register,
            // D-form logical and rotate: destination is RA, not RS.
            20 or 21 or 23 or 24 or 25 or 26 or 27 or 28 or 29 => ra == register,
            // X-form: not worth splitting by sub-opcode, so treat RT or RA as clobbered.
            31 => rt == register || ra == register,
            // Store with update writes the RA base back.
            37 or 39 or 45 => ra == register,
            // lmw loads RT through r31.
            46 => register >= rt,
            // Float load/store with update writes the RA base back; the value
            // register is an FPR.
            49 or 51 or 53 or 55 or 57 or 61 => ra == register,
            _ => false
        };
    }
}
