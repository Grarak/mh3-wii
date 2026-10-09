using System.Collections.Generic;
using System.IO;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;
using Translator.Core.Parsing.Rel;
using Xunit;

namespace Translator.Tests;

public class PpcDisassemblerTests
{
    private static ProgramImage LoadImage()
    {
        var root = ProjectPaths.FindRepositoryRoot();
        var assets = Path.Combine(root, "assets");
        var builder = new ProgramImageBuilder();
        return builder.Build(Path.Combine(assets, "main.dol"), Path.Combine(assets, "StaticR.rel"));
    }

    [Fact]
    public void DisassemblesKnownFunctionWithStableInstructionCount()
    {
        var image = LoadImage();
        using var disassembler = new PpcDisassembler();

        var instructions = disassembler.DisassembleFunction(image, 0x800060A4, maxInstructions: 128);

        Assert.NotEmpty(instructions);
        Assert.True(instructions.Last().IsReturn || instructions.Last().IsUnconditionalBranch || instructions.Last().IsConditionalBranch,
            "Function should terminate with a control-transfer instruction");
        Assert.Equal(0x800060A4u, instructions[0].Address);

        // Regression guard: reachable-walk should pull in the whole control-flow region (was ~90 after CFG-aware disasm).
        Assert.InRange(instructions.Count, 60, 120);
    }

    [Fact]
    public void ThrowsWhenLargeFunctionExceedsLegacyInstructionBudget()
    {
        var image = LoadImage();
        using var disassembler = new PpcDisassembler();

        var ex = Assert.Throws<PpcDisassemblyLimitExceededException>(
            () => disassembler.DisassembleFunction(image, 0x80821E14, maxInstructions: 4096, maxBytes: 0x8000));

        Assert.Contains("0x80821E14", ex.Message);
        Assert.Equal(0x80821E14u, ex.EntryPoint);
        Assert.Equal(4096, ex.MaxInstructions);
        Assert.Equal(0x8000, ex.MaxBytes);
        Assert.True(ex.DecodedInstructions >= 4096);
        Assert.True(ex.BlockedAddress >= ex.EntryPoint);
    }

    [Fact]
    public void DecodesCmpwFromXo0()
    {
        var instruction = PpcDecoder.Decode(0x80000000, 0x7C032800);

        Assert.Equal("cmpw", instruction.Mnemonic);
        Assert.Collection(
            instruction.Operands,
            op => Assert.Equal("cr0", Assert.IsType<PpcConditionRegisterOperand>(op).Name),
            op => Assert.Equal("r3", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal("r5", Assert.IsType<PpcRegisterOperand>(op).Name));
    }

    [Fact]
    public void JumpTableTargetsIgnoreKnownExternalFunctionEntries()
    {
        const uint entry = 0x80000000;
        const uint table = 0x80000100;
        const uint case0 = entry + 0x1C;
        const uint case1 = entry + 0x24;

        var image = TranslatorCppTestHarness.CreateImage(
            (entry + 0x00, 0x28030001), // cmplwi r3, 1
            (entry + 0x04, 0x5463103A), // slwi r3, r3, 2
            (entry + 0x08, 0x3D808000), // lis r12, 0x8000
            (entry + 0x0C, 0x398C0100), // addi r12, r12, 0x100
            (entry + 0x10, 0x7C0C182E), // lwzx r0, r12, r3
            (entry + 0x14, 0x7C0903A6), // mtctr r0
            (entry + 0x18, 0x4E800420), // bctr
            (case0 + 0x00, 0x38600007), // li r3, 7
            (case0 + 0x04, 0x4E800020), // blr
            (case1 + 0x00, 0x38600009), // li r3, 9
            (case1 + 0x04, 0x4E800020), // blr
            (table + 0x00, case0),
            (table + 0x04, case1));

        using var disassembler = new PpcDisassembler();
        var instructions = disassembler.DisassembleFunction(
            image,
            entry,
            maxInstructions: 32,
            maxBytes: 0x80,
            knownFunctionEntryPoints: new HashSet<uint> { entry, case1 });

        Assert.Contains(instructions, ins => ins.Address == case0);
        Assert.Contains(instructions, ins => ins.Address == case1);
    }

    [Fact]
    public void JumpTableCaseBranchCanTargetKnownLocalContinuation()
    {
        const uint entry = 0x80000000;
        const uint table = 0x80000100;
        const uint case0 = entry + 0x1C;
        const uint case1 = entry + 0x28;
        const uint common = entry + 0x34;

        var image = TranslatorCppTestHarness.CreateImage(
            (entry + 0x00, 0x28030002), // cmplwi r3, 2
            (entry + 0x04, 0x5463103A), // slwi r3, r3, 2
            (entry + 0x08, 0x3D808000), // lis r12, 0x8000
            (entry + 0x0C, 0x398C0100), // addi r12, r12, 0x100
            (entry + 0x10, 0x7C0C182E), // lwzx r0, r12, r3
            (entry + 0x14, 0x7C0903A6), // mtctr r0
            (entry + 0x18, 0x4E800420), // bctr
            (case0 + 0x00, 0x38600007), // li r3, 7
            (case0 + 0x04, 0x48000014), // b common
            (case1 + 0x00, 0x38600009), // li r3, 9
            (case1 + 0x04, 0x48000008), // b common
            (common + 0x00, 0x4E800020), // blr
            (table + 0x00, case0),
            (table + 0x04, case1),
            (table + 0x08, common));

        using var disassembler = new PpcDisassembler();
        var instructions = disassembler.DisassembleFunction(
            image,
            entry,
            maxInstructions: 32,
            maxBytes: 0x80,
            knownFunctionEntryPoints: new HashSet<uint> { entry, case0, case1, common });

        Assert.Contains(instructions, ins => ins.Address == common);
    }

    [Fact]
    public void ConditionalBranchesStillFollowKnownFunctionEntryTargets()
    {
        const uint entry = 0x80001000;
        const uint branchTarget = 0x80001014;

        var image = TranslatorCppTestHarness.CreateImage(
            (entry + 0x00, 0x2C030000), // cmpwi r3, 0
            (entry + 0x04, EncodeBc(12, 2, 0x10)), // beq branchTarget
            (entry + 0x08, 0x38800001), // li r4, 1
            (entry + 0x0C, 0x4E800020), // blr
            (branchTarget + 0x00, 0x38800002), // li r4, 2
            (branchTarget + 0x04, 0x4E800020)); // blr

        using var disassembler = new PpcDisassembler();
        var instructions = disassembler.DisassembleFunction(
            image,
            entry,
            maxInstructions: 16,
            maxBytes: 0x40,
            knownFunctionEntryPoints: new HashSet<uint> { entry, branchTarget });

        Assert.Contains(instructions, ins => ins.Address == branchTarget);
    }

    [Fact]
    public void DisassemblerThrowsWhenEntryPointIsOutsideLoadedImage()
    {
        var image = new ProgramImage(
            new byte[8],
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, 8),
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, 8),
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, 0),
            "test");

        using var disassembler = new PpcDisassembler();
        Assert.Throws<ArgumentOutOfRangeException>(
            () => disassembler.DisassembleFunction(image, MemoryLayout.RamBase + 0x100));
    }

    [Fact]
    public void LimitExceededExceptionExposesAllRecordedProperties()
    {
        var ex = new PpcDisassemblyLimitExceededException(
            entryPoint: 0x800060A4,
            maxInstructions: 32,
            maxBytes: 0x100,
            decodedInstructions: 32,
            blockedAddress: 0x80006100);

        Assert.Equal(0x800060A4u, ex.EntryPoint);
        Assert.Equal(32, ex.MaxInstructions);
        Assert.Equal(0x100, ex.MaxBytes);
        Assert.Equal(32, ex.DecodedInstructions);
        Assert.Equal(0x80006100u, ex.BlockedAddress);
        Assert.Contains("0x80006100", ex.Message);
    }

    private static uint EncodeBc(uint bo, uint bi, int branchDelta, bool aa = false, bool lk = false)
        => (16u << 26) | (bo << 21) | (bi << 16) | ((((uint)branchDelta >> 2) & 0x3FFFu) << 2) | ((aa ? 1u : 0u) << 1) | (lk ? 1u : 0u);
}
