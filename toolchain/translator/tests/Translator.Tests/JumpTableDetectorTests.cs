using System.Collections.Generic;
using Translator.Core.Disassembly;
using Xunit;

namespace Translator.Tests;

public class JumpTableDetectorTests
{
    private static IReadOnlyDictionary<uint, int> BuildIndex(IReadOnlyList<PpcInstruction> ordered)
        => ordered.Select((ins, idx) => new { ins.Address, idx }).ToDictionary(x => x.Address, x => x.idx);

    [Fact]
    public void RecognizesLwzxBackedSwitchTable()
    {
        const uint baseAddr = 0x80000000;
        const uint tableAddr = 0x80000100;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(2) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "slwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(2) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x18, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var indexByAddress = BuildIndex(ordered);

        var image = TranslatorCppTestHarness.CreateImage(
            (tableAddr + 0x00, baseAddr + 0x40),
            (tableAddr + 0x04, baseAddr + 0x50),
            (tableAddr + 0x08, baseAddr + 0x60));

        var recognized = JumpTableDetector.TryRecognize(
            ordered,
            indexByAddress,
            baseAddr + 0x18,
            baseAddr,
            baseAddr + 0x100,
            image,
            out var targets);

        Assert.True(recognized);
        Assert.Equal(new uint[] { baseAddr + 0x40, baseAddr + 0x50, baseAddr + 0x60 }, targets);
    }

    [Fact]
    public void UsesFallbackUpperBoundWhenCompareUsesLogicalCounter()
    {
        const uint baseAddr = 0x80001000;
        const uint tableAddr = 0x80001200;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r18", 18), new PpcImmediateOperand(1) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "slwi", new PpcOperand[] { new PpcRegisterOperand("r30", 30), new PpcRegisterOperand("r18", 18), new PpcImmediateOperand(2) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x1200) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x18, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var indexByAddress = BuildIndex(ordered);

        var image = TranslatorCppTestHarness.CreateImage(
            (tableAddr + 0x00, baseAddr + 0x30),
            (tableAddr + 0x04, baseAddr + 0x34));

        var recognized = JumpTableDetector.TryRecognize(
            ordered,
            indexByAddress,
            baseAddr + 0x18,
            baseAddr,
            baseAddr + 0x100,
            image,
            out var targets);

        Assert.True(recognized);
        Assert.Equal(new uint[] { baseAddr + 0x30, baseAddr + 0x34 }, targets);
    }

    [Fact]
    public void RecognizesPcRelativeTablePointerWithRelativeEntries()
    {
        const uint baseAddr = 0x80010000;
        const uint linkAddress = baseAddr + 0x04;
        const uint picBase = 0x80011000;
        const uint tableAddr = 0x80012000;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "bl", new PpcOperand[] { new PpcBranchTargetOperand(linkAddress) }, [linkAddress], isCall: true),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "mflr", new PpcOperand[] { new PpcRegisterOperand("r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "lwz", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcDisplacementOperand(-20, "r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "add", new PpcOperand[] { new PpcRegisterOperand("r30", 30), new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcImmediateOperand(1) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "slwi", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r9", 9), new PpcImmediateOperand(2) }),
            PpcInstruction.Synthetic(baseAddr + 0x18, 0, "lwz", new PpcOperand[] { new PpcRegisterOperand("r10", 10), new PpcDisplacementOperand(-0x20, "r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x1C, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r10", 10), new PpcRegisterOperand("r9", 9) }),
            PpcInstruction.Synthetic(baseAddr + 0x20, 0, "add", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r10", 10) }),
            PpcInstruction.Synthetic(baseAddr + 0x24, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r9", 9) }),
            PpcInstruction.Synthetic(baseAddr + 0x28, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var image = TranslatorCppTestHarness.CreateImage(
            (linkAddress - 20, unchecked(picBase - linkAddress)),
            (picBase - 0x20, tableAddr),
            (tableAddr + 0x00, unchecked((baseAddr + 0x40) - tableAddr)),
            (tableAddr + 0x04, unchecked((baseAddr + 0x50) - tableAddr)));

        var recognized = JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x28,
            baseAddr,
            baseAddr + 0x100,
            image,
            out var targets);

        Assert.True(recognized);
        Assert.Equal(new uint[] { baseAddr + 0x40, baseAddr + 0x50 }, targets);
    }

    [Fact]
    public void RecognizesPcRelativeTablePointerWhenTocSetupIsFarBack()
    {
        const uint baseAddr = 0x80018000;
        const uint linkAddress = baseAddr + 0x04;
        const uint picBase = 0x8001A000;
        const uint tableAddr = 0x8001B000;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "bl", new PpcOperand[] { new PpcBranchTargetOperand(linkAddress) }, [linkAddress], isCall: true),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "mflr", new PpcOperand[] { new PpcRegisterOperand("r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "lwz", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcDisplacementOperand(-20, "r30", 30) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "add", new PpcOperand[] { new PpcRegisterOperand("r30", 30), new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r30", 30) }),
        };

        for (var i = 0; i < 76; i++)
        {
            ordered.Add(PpcInstruction.Synthetic(baseAddr + 0x10 + (uint)(i * 4), 0, "nop", System.Array.Empty<PpcOperand>()));
        }

        var switchBase = baseAddr + 0x10 + (76u * 4);
        ordered.AddRange(new[]
        {
            PpcInstruction.Synthetic(switchBase + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcImmediateOperand(1) }),
            PpcInstruction.Synthetic(switchBase + 0x04, 0, "slwi", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r9", 9), new PpcImmediateOperand(2) }),
            PpcInstruction.Synthetic(switchBase + 0x08, 0, "lwz", new PpcOperand[] { new PpcRegisterOperand("r10", 10), new PpcDisplacementOperand(-0x20, "r30", 30) }),
            PpcInstruction.Synthetic(switchBase + 0x0C, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r10", 10), new PpcRegisterOperand("r9", 9) }),
            PpcInstruction.Synthetic(switchBase + 0x10, 0, "add", new PpcOperand[] { new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r9", 9), new PpcRegisterOperand("r10", 10) }),
            PpcInstruction.Synthetic(switchBase + 0x14, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r9", 9) }),
            PpcInstruction.Synthetic(switchBase + 0x18, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        });

        var image = TranslatorCppTestHarness.CreateImage(
            (linkAddress - 20, unchecked(picBase - linkAddress)),
            (picBase - 0x20, tableAddr),
            (tableAddr + 0x00, unchecked((baseAddr + 0x180) - tableAddr)),
            (tableAddr + 0x04, unchecked((baseAddr + 0x190) - tableAddr)));

        var recognized = JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            switchBase + 0x18,
            baseAddr,
            baseAddr + 0x200,
            image,
            out var targets);

        Assert.True(recognized);
        Assert.Equal(new uint[] { baseAddr + 0x180, baseAddr + 0x190 }, targets);
    }

    [Fact]
    public void RejectsWhenEntryPointOrMtctrChainIsMissing()
    {
        const uint baseAddr = 0x80002000;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var image = TranslatorCppTestHarness.CreateImage((0x80002100u, baseAddr + 0x20));

        Assert.False(JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x10,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));

        Assert.False(JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x04,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));
    }

    [Fact]
    public void RejectsWhenCtrLoadChainIsInvalid()
    {
        const uint baseAddr = 0x80003000;
        const uint tableAddr = 0x80003100;
        var image = TranslatorCppTestHarness.CreateImage((tableAddr + 0x00, baseAddr + 0x20));

        var wrongMtctrOperand = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(0) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "mtctr", System.Array.Empty<PpcOperand>()),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        Assert.False(JumpTableDetector.TryRecognize(
            wrongMtctrOperand,
            BuildIndex(wrongMtctrOperand),
            baseAddr + 0x14,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));

        var wrongLoadDest = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(0) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "lbzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        Assert.False(JumpTableDetector.TryRecognize(
            wrongLoadDest,
            BuildIndex(wrongLoadDest),
            baseAddr + 0x14,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));
    }

    [Fact]
    public void RejectsWhenTableBaseOrUpperBoundCannotBeResolved()
    {
        const uint baseAddr = 0x80004000;
        const uint tableAddr = 0x80004100;
        var image = TranslatorCppTestHarness.CreateImage((tableAddr + 0x00, baseAddr + 0x20));

        var unresolvedBase = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(0) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r11", 11), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        Assert.False(JumpTableDetector.TryRecognize(
            unresolvedBase,
            BuildIndex(unresolvedBase),
            baseAddr + 0x10,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));

        var oversizeBound = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(600) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        Assert.False(JumpTableDetector.TryRecognize(
            oversizeBound,
            BuildIndex(oversizeBound),
            baseAddr + 0x14,
            baseAddr,
            baseAddr + 0x100,
            image,
            out _));
    }

    [Fact]
    public void RejectsUnreadableAndOutOfWindowTargets()
    {
        const uint baseAddr = 0x80005000;
        const uint tableAddr = 0x80005100;
        var ordered = new List<PpcInstruction>
        {
            PpcInstruction.Synthetic(baseAddr + 0x00, 0, "cmplwi", new PpcOperand[] { new PpcRegisterOperand("r3", 3), new PpcImmediateOperand(1) }),
            PpcInstruction.Synthetic(baseAddr + 0x04, 0, "lis", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(unchecked((short)0x8000)) }),
            PpcInstruction.Synthetic(baseAddr + 0x08, 0, "addi", new PpcOperand[] { new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r12", 12), new PpcImmediateOperand(0x0100) }),
            PpcInstruction.Synthetic(baseAddr + 0x0C, 0, "lwzx", new PpcOperand[] { new PpcRegisterOperand("r0", 0), new PpcRegisterOperand("r12", 12), new PpcRegisterOperand("r3", 3) }),
            PpcInstruction.Synthetic(baseAddr + 0x10, 0, "mtctr", new PpcOperand[] { new PpcRegisterOperand("r0", 0) }),
            PpcInstruction.Synthetic(baseAddr + 0x14, 0, "bctr", System.Array.Empty<PpcOperand>(), isReturn: false, isCall: false, isConditional: false),
        };

        var missingEntryImage = TranslatorCppTestHarness.CreateImage((tableAddr + 0x00, baseAddr + 0x20));
        Assert.False(JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x14,
            baseAddr,
            baseAddr + 0x100,
            missingEntryImage,
            out _));

        var outOfWindowImage = TranslatorCppTestHarness.CreateImage(
            (tableAddr + 0x00, 0x90000000u),
            (tableAddr + 0x04, baseAddr + 0x20));
        Assert.False(JumpTableDetector.TryRecognize(
            ordered,
            BuildIndex(ordered),
            baseAddr + 0x14,
            baseAddr,
            baseAddr + 0x100,
            outOfWindowImage,
            out _));
    }
}
