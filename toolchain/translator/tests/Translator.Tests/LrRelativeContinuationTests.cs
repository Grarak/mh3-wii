using System.Buffers.Binary;
using Translator.Core.Disassembly;
using Translator.Core.Loading;
using Translator.Core.Mods;
using Xunit;

namespace Translator.Tests;

public class LrRelativeContinuationTests
{
    [Theory]
    [InlineData(20, 40)]
    [InlineData(40, 20)]
    public void MutuallyExclusiveAdjustmentsKeepBothOffsets(int firstOffset, int secondOffset)
    {
        // Both arms start with the incoming LR and join at mtlr. Adding the
        // offsets together invents a continuation that neither arm can reach.
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // +00: mflr r31
            0x2C030000u, // +04: cmpwi r3,0
            0x4182000Cu, // +08: beq +0x14
            AddiR31(firstOffset), // +0C: addi r31,r31,firstOffset
            0x48000008u, // +10: b +0x18
            AddiR31(secondOffset), // +14: addi r31,r31,secondOffset
            0x7FE803A6u, // +18: mtlr r31
            0x4E800020u);// +1C: blr

        Assert.Equal(new[] { 20, 40 }, offsets);
    }

    [Fact]
    public void NormalReturnArmDoesNotEraseSkipReturnAtSharedBlr()
    {
        // The normal arm writes the original LR; it must not overwrite the
        // other arm's LR + 20 in the analysis of the shared return.
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // +00: mflr r31
            0x2C030000u, // +04: cmpwi r3,0
            0x41820010u, // +08: beq +0x18
            0x397F0014u, // +0C: addi r11,r31,20
            0x7D6803A6u, // +10: mtlr r11
            0x48000008u, // +14: b +0x1C
            0x7FE803A6u, // +18: mtlr r31
            0x4E800020u);// +1C: blr

        Assert.Equal(new[] { 20 }, offsets);
    }

    [Fact]
    public void ConditionalNormalReturnStillDiscoversSkipOnFallthrough()
    {
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x2C030000u, // cmpwi r3,0
            0x4D820020u, // beqlr
            0x3BFF0014u, // addi r31,r31,20
            0x7FE803A6u, // mtlr r31
            0x4E800020u);// blr

        Assert.Equal(new[] { 20 }, offsets);
    }

    [Fact]
    public void SavedNonvolatileLrSurvivesHelperCall()
    {
        // r31 survives a normal ABI call even though the call replaces LR.
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x48000101u, // bl helper outside this function
            0x3BFF0014u, // addi r31,r31,20
            0x7FE803A6u, // mtlr r31
            0x4E800020u);// blr

        Assert.Equal(new[] { 20 }, offsets);
    }

    [Fact]
    public void ReloadingSavedRegisterAfterMtlrDoesNotEraseSkipReturn()
    {
        // A hook epilogue restores the caller's r31 after committing its
        // adjusted return address to LR.
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x3BFF0014u, // addi r31,r31,20
            0x7FE803A6u, // mtlr r31
            0x83E10008u, // lwz r31,8(r1)
            0x4E800020u);// blr

        Assert.Equal(new[] { 20 }, offsets);
    }

    [Fact]
    public void UnknownLrWriteReplacesEarlierSkipReturn()
    {
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x3BFF0014u, // addi r31,r31,20
            0x7FE803A6u, // mtlr r31
            0x80010008u, // lwz r0,8(r1)
            0x7C0803A6u, // mtlr r0
            0x4E800020u);// blr

        Assert.Empty(offsets);
    }

    [Fact]
    public void UnadjustedRegisterReturnDoesNotAddAContinuation()
    {
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x7FE803A6u, // mtlr r31
            0x4E800020u);// blr

        Assert.Empty(offsets);
    }

    [Fact]
    public void LoadMultipleWordOverwritesSavedRegistersThroughR31()
    {
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x3BFF0014u, // addi r31,r31,20
            0xBB610008u, // lmw r30,8(r1)
            0x7FE803A6u, // mtlr r31
            0x4E800020u);// blr

        Assert.Empty(offsets);
    }

    [Fact]
    public void BlrlCallIsNotTreatedAsReturn()
    {
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x4E800021u, // blrl
            0x3BFF0014u, // addi r31,r31,20
            0x7FE803A6u, // mtlr r31
            0x4E800020u);// blr

        Assert.Equal(new[] { 20 }, offsets);
    }

    [Fact]
    public void MflrAfterCallDoesNotTreatClobberedLrAsIncomingLr()
    {
        var offsets = DiscoverOffsets(
            0x48000101u, // bl helper
            0x7FE802A6u, // mflr r31
            0x3BFF0014u, // addi r31,r31,20
            0x7FE803A6u, // mtlr r31
            0x4E800020u);// blr

        Assert.Empty(offsets);
    }

    [Fact]
    public void RestoredIncomingLrBeforeCtrSkipStillDiscoversOffset()
    {
        // The helper replaces LR, but the stack save/restore recovers the
        // incoming LR before the hook jumps to the caller's continuation.
        var offsets = DiscoverOffsets(
            0x7C0802A6u, // mflr r0
            0x90010004u, // stw r0,4(r1)
            0x9421FFF0u, // stwu r1,-16(r1)
            0x48000101u, // bl helper outside this function
            0x38210010u, // addi r1,r1,16
            0x80010004u, // lwz r0,4(r1)
            0x7C0803A6u, // mtlr r0
            0x7D6802A6u, // mflr r11
            0x396B0008u, // addi r11,r11,8
            0x7D6903A6u, // mtctr r11
            0x4E800420u);// bctr

        Assert.Equal(new[] { 8 }, offsets);
    }

    [Fact]
    public void BoundedLoopBeforeCtrSkipStillDiscoversOffset()
    {
        // Updating an LR-derived register in a two-iteration loop must not
        // starve analysis of the exit, whose target uses unchanged r31.
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x7FC802A6u, // mflr r30
            0x38600002u, // li r3,2
            0x7C6903A6u, // mtctr r3
            0x3BDE0004u, // addi r30,r30,4
            0x4200FFFCu, // bdnz -4
            0x397F0008u, // addi r11,r31,8
            0x7D6903A6u, // mtctr r11
            0x4E800420u);// bctr

        Assert.Equal(new[] { 8 }, offsets);
    }

    [Fact]
    public void UntrackedR1WriteInvalidatesStackTracking()
    {
        // If r1 is overwritten from an untracked source, previously saved stack slots
        // must not be used to recover LR state.
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x3BFF0014u, // addi r31,r31,20
            0x93E10008u, // stw r31,8(r1)
            0x80230000u, // lwz r1,0(r3)
            0x80010008u, // lwz r0,8(r1)
            0x7C0803A6u, // mtlr r0
            0x4E800020u);// blr

        Assert.Empty(offsets);
    }

    [Fact]
    public void LargeStraightLineHandlerStillDiscoversSkipReturn()
    {
        // The analyzer's global step budget is spent one step per (instruction,
        // state) pair, so a long enough handler exhausts it before reaching the
        // return and silently reports no continuation at all. Main's linear
        // scanner had no budget and always found the offset.
        var words = new List<uint>
        {
            0x7FE802A6u, // mflr r31
            0x3BFF0014u  // addi r31,r31,20
        };
        for (var i = 0; i < 10_010; i++)
        {
            words.Add(0x60000000u); // nop
        }
        words.Add(0x7FE803A6u); // mtlr r31
        words.Add(0x4E800020u); // blr

        Assert.Equal(new[] { 20 }, DiscoverOffsets(words.ToArray()));
    }

    [Fact]
    public void LargeBranchingHandlerStillDiscoversCtrSkip()
    {
        // Same budget, reached far sooner once the handler branches: this is the
        // bctr shape the pre-PR scanner discovered at any function size.
        var words = new List<uint> { 0x7FE802A6u }; // mflr r31
        for (var i = 0; i < 160; i++)
        {
            var displacement = (uint)((i + 1) * 4 & 0xFFFF);
            words.Add(0x2C030000u);                  // cmpwi r3,0
            words.Add(0x4182000Cu);                  // beq +0xC
            words.Add(0x3BDF0000u | displacement);   // addi r30,r31,disp
            words.Add(0x48000008u);                  // b +8
            words.Add(0x3BBF0000u | displacement);   // addi r29,r31,disp
        }
        words.Add(0x397F0008u); // addi r11,r31,8
        words.Add(0x7D6903A6u); // mtctr r11
        words.Add(0x4E800420u); // bctr

        Assert.Equal(new[] { 8 }, DiscoverOffsets(words.ToArray()));
    }

    [Fact]
    public void FloatStoreOverSavedSlotInvalidatesStackTracking()
    {
        // stfd writes 0x10..0x17, which covers the slot the adjusted LR was
        // saved to. Only stw/stwu invalidate slots today, so the reload is
        // credited with a return address the stack no longer holds.
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x3BFF0014u, // addi r31,r31,20
            0x93E10014u, // stw r31,0x14(r1)
            0xD8410010u, // stfd f2,0x10(r1)
            0x80010014u, // lwz r0,0x14(r1)
            0x7C0803A6u, // mtlr r0
            0x4E800020u);// blr

        Assert.Empty(offsets);
    }

    [Fact]
    public void StoreMultipleOverSavedSlotInvalidatesStackTracking()
    {
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x3BFF0014u, // addi r31,r31,20
            0x93E10008u, // stw r31,8(r1)
            0xBFC10008u, // stmw r30,8(r1)
            0x80010008u, // lwz r0,8(r1)
            0x7C0803A6u, // mtlr r0
            0x4E800020u);// blr

        Assert.Empty(offsets);
    }

    [Fact]
    public void StackPointerUpdatePreservesAdjustedLrOffset()
    {
        var offsets = DiscoverOffsets(
            0x7C2802A6u, // mflr r1
            0xDC410004u, // stfdu f2,4(r1)
            0x7C2803A6u, // mtlr r1
            0x4E800020u);// blr

        Assert.Equal(new[] { 4 }, offsets);
    }

    [Fact]
    public void VolatileRegisterDoesNotSurviveHelperCall()
    {
        // r3 is caller-saved, so the callee is free to destroy the adjusted
        // return address this hook staged before the call.
        var offsets = DiscoverOffsets(
            0x7C6802A6u, // mflr r3
            0x38630014u, // addi r3,r3,20
            0x48000101u, // bl helper outside this function
            0x7C6803A6u, // mtlr r3
            0x4E800020u);// blr

        Assert.Empty(offsets);
    }

    [Fact]
    public void CtrDoesNotSurviveHelperCall()
    {
        // CTR is volatile across a call for the same reason.
        var offsets = DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x3BFF0014u, // addi r31,r31,20
            0x7FE903A6u, // mtctr r31
            0x48000101u, // bl helper outside this function
            0x4E800420u);// bctr

        Assert.Empty(offsets);
    }

    [Fact]
    public void MflrR1InvalidatesOldStackSlots()
    {
        // After mflr r1, 8(r1) refers to incoming LR + 8, not the old
        // stack slot. Its contents are unknown; do not invent a +20 return.
        Assert.Empty(DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x3BFF0014u, // addi r31,r31,20
            0x93E10008u, // stw r31,8(r1)
            0x7C2802A6u, // mflr r1
            0x80010008u, // lwz r0,8(r1)
            0x7C0803A6u, // mtlr r0
            0x4E800020u)); // blr
    }

    [Fact]
    public void AddiR1UpdatesLrRelativeOffset()
    {
        // Like StackPointerUpdatePreservesAdjustedLrOffset, r1 holds incoming
        // LR here. Updating r1 must update that relation as well as stack state.
        Assert.Equal(new[] { 4 }, DiscoverOffsets(
            0x7C2802A6u, // mflr r1
            0x38210004u, // addi r1,r1,4
            0x7C2803A6u, // mtlr r1
            0x4E800020u)); // blr
    }

    [Theory]
    [InlineData(0x38210004u, 4)] // addi r1,r1,4
    [InlineData(0x30210004u, 4)] // addic r1,r1,4
    [InlineData(0x94210004u, 4)] // stwu r1,4(r1)
    [InlineData(0xD4410004u, 4)] // stfsu f2,4(r1)
    [InlineData(0xDC410004u, 4)] // stfdu f2,4(r1)
    [InlineData(0x3821FFFCu, -4)] // addi r1,r1,-4
    public void StackPointerUpdatesPreserveLrRelation(uint update, int expectedOffset)
    {
        Assert.Equal(new[] { expectedOffset }, DiscoverOffsets(
            0x7C2802A6u, // mflr r1
            update,
            0x7C2803A6u, // mtlr r1
            0x4E800020u)); // blr
    }

    [Theory]
    [InlineData(0x7FE1FB78u)] // mr r1,r31
    [InlineData(0x383F0000u)] // addi r1,r31,0
    public void CopyingLrIntoR1PreservesReturnButInvalidatesOldStack(uint copy)
    {
        var prefix = new uint[]
        {
            0x7FE802A6u, // mflr r31
            0x3BFF0014u, // addi r31,r31,20
            0x93E10008u, // stw r31,8(r1)
            copy,
        };
        Assert.Equal(new[] { 20 }, DiscoverOffsets(
            prefix.Concat(new uint[] { 0x7C2803A6u, 0x4E800020u }).ToArray()));
        Assert.Empty(DiscoverOffsets(prefix.Concat(new uint[]
        {
            0x80010008u, // lwz r0,8(r1): no longer the old stack slot
            0x7C0803A6u, // mtlr r0
            0x4E800020u,
        }).ToArray()));
    }

    [Fact]
    public void StackPointerSelfMovePreservesSavedLr()
    {
        Assert.Equal(new[] { 20 }, DiscoverOffsets(
            0x7FE802A6u, // mflr r31
            0x3BFF0014u, // addi r31,r31,20
            0x93E10008u, // stw r31,8(r1)
            0x7C210B78u, // mr r1,r1
            0x80010008u, // lwz r0,8(r1)
            0x7C0803A6u, // mtlr r0
            0x4E800020u));
    }

    [Fact]
    public void IncompleteInstructionListDoesNotInventFallthroughAcrossGap()
    {
        // Defensive incomplete-input test, not a production disassembly trace:
        // the missing instruction could overwrite r31 or branch elsewhere.
        // Address sorting alone does not establish a fallthrough edge.
        var instructions = new[]
        {
            PpcDecoder.Decode(0x81800000u, 0x7FE802A6u), // mflr r31
            PpcDecoder.Decode(0x81800008u, 0x3BFF0014u), // addi r31,r31,20
            PpcDecoder.Decode(0x8180000Cu, 0x7FE803A6u), // mtlr r31
            PpcDecoder.Decode(0x81800010u, 0x4E800020u), // blr
        };

        Assert.Empty(ContinuationPlanner.DiscoverLrRelativeIndirectJumpOffsets(instructions));
    }

    private static uint AddiR31(int offset) => 0x3BFF0000u | (uint)(offset & 0xFFFF);

    private static int[] DiscoverOffsets(params uint[] words)
    {
        const uint entry = 0x81800000u;
        var memory = new byte[words.Length * 4];
        for (var i = 0; i < words.Length; i++)
        {
            BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(i * 4, 4), words[i]);
        }

        var range = AddressRange.FromStartAndSize(entry, (uint)memory.Length);
        var image = new ProgramImage(memory, range, range, default, "lr-continuation-test", entry);
        using var disassembler = new PpcDisassembler();
        // Use the production reachable-instruction traversal and ordering,
        // rather than handing the planner an artificial execution trace.
        var instructions = disassembler.DisassembleFunction(
            image, entry, maxInstructions: words.Length + 1, maxBytes: memory.Length);

        return ContinuationPlanner.DiscoverLrRelativeIndirectJumpOffsets(instructions)
            .Distinct().OrderBy(offset => offset).ToArray();
    }
}
