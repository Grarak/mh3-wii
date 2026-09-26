using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using Translator.Core.Loading;

namespace Translator.Core.Mods.Mkwii;

public sealed record RetroWfcPayloadSummary(
    string? Source,
    string Game,
    uint FormatVersion,
    uint PayloadVersion,
    string PayloadSha256,
    uint HelperModuleOffset,
    uint HelperModuleBase,
    int PayloadImageSize,
    uint InitializationTargetAddress,
    uint InitializationSuccessReturnValue,
    uint ConstructorsStartOffset,
    uint ConstructorsEndOffset,
    uint ExecutableStartOffset,
    uint ExecutableEndOffset,
    IReadOnlyList<RetroWfcHelperInitCallback> InitializationCallbacks)
{
}

public sealed record RetroWfcPayloadLoadResult(
    RetroWfcPayloadSummary Summary,
    byte[] RelocatedImage,
    RetroWfcContractLoweringPlan LoweringPlan);

/// <summary>
/// Reads the normal WWFC payload format directly. The only recomp-specific
/// extension is the static-consumer descriptor referenced by payload info v3.
/// </summary>
public static class RetroWfcPayload
{
    private const int HeaderInfoOffset = 0x130;
    private const int InfoSizeV3 = 0x74;
    private const int PatchRecordSize = 0x10;
    private const uint SupportedPayloadFormat = 3;
    private const uint SupportedStaticConsumerFormat = 1;

    private sealed record PendingBranchBack(int RecordIndex, uint PayloadOffset, uint DestinationAddress);

    public static RetroWfcPayloadLoadResult Parse(
        byte[] payloadImage,
        BaseManifest baseManifest,
        uint moduleGuestBase,
        uint helperModuleOffset,
        string? source = null)
    {
        ArgumentNullException.ThrowIfNull(payloadImage);
        ArgumentNullException.ThrowIfNull(baseManifest);

        RequireRange(payloadImage, 0, HeaderInfoOffset + InfoSizeV3, "payload header");
        if (!payloadImage.AsSpan(0, 12).SequenceEqual("WWFC/Payload"u8))
        {
            throw new InvalidDataException("Retro WFC payload has an invalid magic value.");
        }

        var totalSize = ReadU32(payloadImage, 0x0C);
        if (totalSize != payloadImage.Length)
        {
            throw new InvalidDataException(
                $"Retro WFC payload header size 0x{totalSize:X} does not match the supplied image size 0x{payloadImage.Length:X}.");
        }

        var formatVersion = ReadInfo(payloadImage, 0x00);
        var compatibleVersion = ReadInfo(payloadImage, 0x04);
        if (formatVersion < SupportedPayloadFormat || compatibleVersion > SupportedPayloadFormat)
        {
            throw new InvalidDataException(
                $"Retro WFC payload format {formatVersion} (compatible with {compatibleVersion}) cannot be read by format {SupportedPayloadFormat}.");
        }

        var game = Encoding.ASCII.GetString(payloadImage, HeaderInfoOffset + 0x08, 0x0C).TrimEnd('\0');
        ValidateGame(game, baseManifest.GameId);
        for (var offset = HeaderInfoOffset + 0x3C; offset < HeaderInfoOffset + 0x50; offset += 4)
        {
            if (ReadU32(payloadImage, offset) != 0)
            {
                throw new InvalidDataException($"Retro WFC payload reserved info word at 0x{offset:X} must be zero.");
            }
        }

        var gotStart = ReadInfo(payloadImage, 0x18);
        var gotEnd = ReadInfo(payloadImage, 0x1C);
        var fixupStart = ReadInfo(payloadImage, 0x20);
        var fixupEnd = ReadInfo(payloadImage, 0x24);
        var patchStart = ReadInfo(payloadImage, 0x28);
        var patchEnd = ReadInfo(payloadImage, 0x2C);
        var staticInfoOffset = ReadInfo(payloadImage, 0x70);
        ValidateAlignedRange(payloadImage, gotStart, gotEnd, 4, "GOT");
        ValidateAlignedRange(payloadImage, fixupStart, fixupEnd, 4, "fixup table");
        ValidateAlignedRange(payloadImage, patchStart, patchEnd, PatchRecordSize, "patch list");

        RequireRange(payloadImage, staticInfoOffset, 0x10, "static-consumer descriptor");
        var staticInfoVersion = ReadU32(payloadImage, staticInfoOffset);
        if (staticInfoVersion != SupportedStaticConsumerFormat)
        {
            throw new InvalidDataException(
                $"Retro WFC static-consumer descriptor format {staticInfoVersion} is not supported.");
        }

        var patchFreeEntry = ReadU32(payloadImage, checked((int)staticInfoOffset + 0x04));
        var constructorsStart = patchEnd;
        var constructorsEnd = ReadU32(payloadImage, checked((int)staticInfoOffset + 0x08));
        var executableStart = constructorsEnd;
        var executableEnd = ReadU32(payloadImage, checked((int)staticInfoOffset + 0x0C));
        ValidateAlignedRange(payloadImage, constructorsStart, constructorsEnd, 4, "constructor table");
        ValidateAlignedRange(payloadImage, executableStart, executableEnd, 4, "executable payload");
        RequireExecutableOffset(patchFreeEntry, executableStart, executableEnd, "patch-free entry point");

        var helperModuleBase = checked(moduleGuestBase + helperModuleOffset);
        var semanticTargets = new Dictionary<string, RetroWfcSemanticActionTarget>(StringComparer.Ordinal);
        var contractRecords = new List<Dictionary<string, object?>>();
        var pendingBranchBacks = new List<PendingBranchBack>();
        var recordCount = checked((int)((patchEnd - patchStart) / PatchRecordSize));

        for (var index = 0; index < recordCount; index++)
        {
            var offset = checked((int)patchStart + index * PatchRecordSize);
            var level = payloadImage[offset];
            var type = payloadImage[offset + 1];
            var address = ReadU32(payloadImage, offset + 4);
            var arg0 = ReadU32(payloadImage, offset + 8);
            var arg1 = ReadU32(payloadImage, offset + 12);
            if ((level & 0x10) != 0 || address == 0)
            {
                continue;
            }

            switch (type)
            {
                case 0:
                    AddStaticBytePatch(payloadImage, contractRecords, index, address, arg0, arg1);
                    break;
                case 1:
                case 2:
                case 3:
                case 4:
                case 5:
                    AddExecutablePatch(
                        payloadImage,
                        baseManifest,
                        semanticTargets,
                        contractRecords,
                        pendingBranchBacks,
                        index,
                        type,
                        address,
                        arg0,
                        arg1,
                        helperModuleOffset,
                        helperModuleBase,
                        executableStart,
                        executableEnd);
                    break;
                case 6:
                    AddStaticPointerPatch(
                        payloadImage,
                        baseManifest,
                        semanticTargets,
                        contractRecords,
                        index,
                        address,
                        arg0,
                        helperModuleOffset,
                        helperModuleBase,
                        executableStart,
                        executableEnd);
                    break;
                default:
                    throw new InvalidDataException($"Retro WFC patch #{index} has unsupported type {type}.");
            }
        }

        var relocatedImage = payloadImage.ToArray();
        RelocatePayload(relocatedImage, helperModuleBase, gotStart, gotEnd, fixupStart, fixupEnd);
        foreach (var branchBack in pendingBranchBacks)
        {
            var branchAddress = checked(helperModuleBase + branchBack.PayloadOffset);
            var instruction = 0x48000000u | ((branchBack.DestinationAddress - branchAddress) & 0x03FFFFFCu);
            WriteU32(relocatedImage, branchBack.PayloadOffset, instruction);
        }

        var callbacks = ReadConstructors(
            payloadImage,
            helperModuleOffset,
            helperModuleBase,
            constructorsStart,
            constructorsEnd,
            executableStart,
            executableEnd);
        var contractJson = JsonSerializer.Serialize(new { patchRecords = contractRecords });
        var loweringPlan = RetroWfcContractLoweringPlanner.Build(
            contractJson,
            baseManifest,
            source,
            semanticTargets);
        if (!loweringPlan.IsPlannable)
        {
            throw new InvalidDataException(
                "Retro WFC payload patch list could not be lowered statically." +
                Environment.NewLine +
                loweringPlan.BuildTextReport());
        }

        var summary = new RetroWfcPayloadSummary(
            source,
            game,
            formatVersion,
            ReadInfo(payloadImage, 0x14),
            ChecksumUtilities.Sha256Hex(payloadImage),
            helperModuleOffset,
            helperModuleBase,
            payloadImage.Length,
            checked(helperModuleBase + patchFreeEntry),
            0,
            constructorsStart,
            constructorsEnd,
            executableStart,
            executableEnd,
            callbacks);
        return new RetroWfcPayloadLoadResult(summary, relocatedImage, loweringPlan);
    }

    private static void AddStaticBytePatch(
        byte[] payloadImage,
        List<Dictionary<string, object?>> records,
        int index,
        uint address,
        uint sourceOffset,
        uint size)
    {
        RequireRange(payloadImage, sourceOffset, size, $"patch #{index} source bytes");
        var bytes = payloadImage.AsSpan(checked((int)sourceOffset), checked((int)size));
        records.Add(new Dictionary<string, object?>
        {
            ["index"] = index,
            ["intent"] = "staticBytes",
            ["typeName"] = "write",
            ["address"] = $"0x{address:X8}",
            ["sourceBytes"] = new
            {
                kind = "payloadSlice",
                hex = ChecksumUtilities.ToHex(bytes),
                sha256 = ChecksumUtilities.Sha256Hex(bytes),
            },
            ["target"] = new
            {
                resolvedBy = "sourceBytes",
                resolutionRequired = false,
            },
        });
    }

    private static void AddExecutablePatch(
        byte[] payloadImage,
        BaseManifest baseManifest,
        Dictionary<string, RetroWfcSemanticActionTarget> semanticTargets,
        List<Dictionary<string, object?>> records,
        List<PendingBranchBack> pendingBranchBacks,
        int index,
        byte type,
        uint address,
        uint targetValue,
        uint arg1,
        uint helperModuleOffset,
        uint helperModuleBase,
        uint executableStart,
        uint executableEnd)
    {
        var (targetAddress, targetKind, moduleOffset) = ResolveTarget(
            payloadImage,
            baseManifest,
            targetValue,
            helperModuleOffset,
            helperModuleBase,
            executableStart,
            executableEnd,
            requireExecutable: true,
            $"patch #{index} branch target");
        if ((type is 4 or 5) && arg1 > 31)
        {
            throw new InvalidDataException($"Retro WFC patch #{index} uses invalid CTR scratch register {arg1}.");
        }

        if (type == 2)
        {
            if ((arg1 & 0x80000000u) != 0)
            {
                throw new InvalidDataException(
                    $"Retro WFC patch #{index} writes its branch-back outside the payload; static lowering does not support that layout.");
            }
            RequireExecutableOffset(arg1, executableStart, executableEnd, $"patch #{index} branch-back");
            pendingBranchBacks.Add(new PendingBranchBack(index, arg1, checked(address + 4)));
        }

        var actionId = $"payload.patch.{index}.target";
        semanticTargets.Add(
            actionId,
            new RetroWfcSemanticActionTarget(actionId, targetKind, targetAddress, moduleOffset, null, "wwfc-payload"));
        var (intent, typeName, instructionCount) = type switch
        {
            1 => ("executableHook", "branch", 1),
            2 => ("executableHookWithContinuation", "branchHook", 1),
            3 => ("executableCall", "call", 1),
            4 => ("executableCtrHook", "branchCtr", 4),
            5 => ("executableCtrCall", "branchCtrLink", 4),
            _ => throw new InvalidOperationException(),
        };
        records.Add(new Dictionary<string, object?>
        {
            ["index"] = index,
            ["intent"] = intent,
            ["typeName"] = typeName,
            ["address"] = $"0x{address:X8}",
            ["instructionCount"] = instructionCount,
            ["semanticActionIds"] = new[] { actionId },
        });
    }

    private static void AddStaticPointerPatch(
        byte[] payloadImage,
        BaseManifest baseManifest,
        Dictionary<string, RetroWfcSemanticActionTarget> semanticTargets,
        List<Dictionary<string, object?>> records,
        int index,
        uint address,
        uint targetValue,
        uint helperModuleOffset,
        uint helperModuleBase,
        uint executableStart,
        uint executableEnd)
    {
        var (targetAddress, targetKind, moduleOffset) = ResolveTarget(
            payloadImage,
            baseManifest,
            targetValue,
            helperModuleOffset,
            helperModuleBase,
            executableStart,
            executableEnd,
            requireExecutable: false,
            $"patch #{index} pointer target");
        var actionId = $"payload.patch.{index}.target";
        semanticTargets.Add(
            actionId,
            new RetroWfcSemanticActionTarget(actionId, targetKind, targetAddress, moduleOffset, null, "wwfc-payload"));
        records.Add(new Dictionary<string, object?>
        {
            ["index"] = index,
            ["intent"] = "staticPointer",
            ["typeName"] = "writePointer",
            ["address"] = $"0x{address:X8}",
            ["semanticActionIds"] = new[] { actionId },
        });
    }

    private static (uint Address, string Kind, uint ModuleOffset) ResolveTarget(
        byte[] payloadImage,
        BaseManifest baseManifest,
        uint value,
        uint helperModuleOffset,
        uint helperModuleBase,
        uint executableStart,
        uint executableEnd,
        bool requireExecutable,
        string description)
    {
        if ((value & 0x80000000u) == 0)
        {
            RequireRange(payloadImage, value, requireExecutable ? 4u : 1u, description);
            var executable = value >= executableStart && value < executableEnd;
            if (requireExecutable && !executable)
            {
                throw new InvalidDataException(
                    $"Retro WFC {description} offset 0x{value:X} is outside the declared executable range.");
            }
            return (
                checked(helperModuleBase + value),
                executable ? "moduleFunction" : "moduleData",
                checked(helperModuleOffset + value));
        }

        var section = baseManifest.Sections.FirstOrDefault(s => value >= s.GuestStart && value < s.GuestEnd);
        if (section is null || (requireExecutable && !section.Executable))
        {
            throw new InvalidDataException(
                $"Retro WFC {description} address 0x{value:X8} is outside a compatible base section.");
        }
        return (value, section.Executable ? "baseFunction" : "baseData", 0);
    }

    private static IReadOnlyList<RetroWfcHelperInitCallback> ReadConstructors(
        byte[] payloadImage,
        uint helperModuleOffset,
        uint helperModuleBase,
        uint start,
        uint end,
        uint executableStart,
        uint executableEnd)
    {
        var callbacks = new List<RetroWfcHelperInitCallback>();
        for (var offset = start; offset < end; offset += 4)
        {
            var targetOffset = ReadU32(payloadImage, offset);
            if (targetOffset is 0 or 0xFFFFFFFFu)
            {
                continue;
            }
            RequireExecutableOffset(targetOffset, executableStart, executableEnd, "constructor");
            callbacks.Add(new RetroWfcHelperInitCallback(
                "constructor",
                checked(helperModuleBase + targetOffset),
                checked(helperModuleOffset + targetOffset),
                null));
        }
        return callbacks;
    }

    private static void RelocatePayload(
        byte[] image,
        uint payloadBase,
        uint gotStart,
        uint gotEnd,
        uint fixupStart,
        uint fixupEnd)
    {
        for (var offset = gotStart; offset < gotEnd; offset += 4)
        {
            RelocateWord(image, offset, payloadBase);
        }

        for (var offset = fixupStart; offset < fixupEnd; offset += 4)
        {
            var targetOffset = ReadU32(image, offset);
            RequireRange(image, targetOffset, 4, "fixup target");
            RelocateWord(image, targetOffset, payloadBase);
        }
    }

    private static void RelocateWord(byte[] image, uint offset, uint payloadBase)
    {
        var value = ReadU32(image, offset);
        if ((value & 0x80000000u) == 0)
        {
            WriteU32(image, offset, checked(payloadBase + value));
        }
    }

    private static uint ReadInfo(byte[] image, int relativeOffset) =>
        ReadU32(image, HeaderInfoOffset + relativeOffset);

    private static uint ReadU32(byte[] image, uint offset) =>
        ReadU32(image, checked((int)offset));

    private static uint ReadU32(byte[] image, int offset)
    {
        RequireRange(image, offset, 4, "32-bit value");
        return BinaryPrimitives.ReadUInt32BigEndian(image.AsSpan(offset, 4));
    }

    private static void WriteU32(byte[] image, uint offset, uint value)
    {
        RequireRange(image, offset, 4, "32-bit value");
        BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(checked((int)offset), 4), value);
    }

    private static void ValidateGame(string payloadGame, string baseGame)
    {
        if (payloadGame.Length < 5 ||
            baseGame.Length < 4 ||
            !payloadGame.StartsWith(baseGame[..4], StringComparison.OrdinalIgnoreCase) ||
            payloadGame[4] != 'D')
        {
            throw new InvalidDataException(
                $"Retro WFC payload game '{payloadGame}' is not compatible with base game '{baseGame}'.");
        }
    }

    private static void ValidateAlignedRange(
        byte[] image,
        uint start,
        uint end,
        int elementSize,
        string description)
    {
        if (end < start || (start & 3u) != 0 || ((end - start) % elementSize) != 0)
        {
            throw new InvalidDataException($"Retro WFC payload {description} range 0x{start:X}-0x{end:X} is malformed.");
        }
        RequireRange(image, start, end - start, description);
    }

    private static void RequireExecutableOffset(uint offset, uint start, uint end, string description)
    {
        if ((offset & 3) != 0 || offset < start || offset >= end)
        {
            throw new InvalidDataException(
                $"Retro WFC payload {description} offset 0x{offset:X} is outside executable range 0x{start:X}-0x{end:X}.");
        }
    }

    private static void RequireRange(byte[] image, uint offset, uint size, string description)
    {
        if (offset > image.Length || size > image.Length - offset)
        {
            throw new InvalidDataException(
                $"Retro WFC payload {description} range 0x{offset:X}+0x{size:X} is outside the image.");
        }
    }

    private static void RequireRange(byte[] image, int offset, int size, string description)
    {
        if (offset < 0 || size < 0 || offset > image.Length || size > image.Length - offset)
        {
            throw new InvalidDataException(
                $"Retro WFC payload {description} range 0x{offset:X}+0x{size:X} is outside the image.");
        }
    }
}
