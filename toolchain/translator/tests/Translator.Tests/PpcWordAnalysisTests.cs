using Translator.Core.Disassembly;
using Xunit;

namespace Translator.Tests;

public sealed class PpcWordAnalysisTests
{
    [Fact]
    public void FieldsExposeSplitSprAndImmediateForms()
    {
        var fields = new PpcWordFields(0x7D8903A6u); // mtctr r12

        Assert.Equal(31, fields.PrimaryOpcode);
        Assert.Equal(12, fields.GprField0);
        Assert.Equal(9, fields.Spr);
        Assert.Equal(467, fields.ExtendedOpcode);

        var immediateFields = new PpcWordFields(0x398CFFF0u); // addi r12,r12,-0x10
        Assert.Equal(-16, immediateFields.SignedImmediate16);
        Assert.Equal(0xFFF0u, immediateFields.UnsignedImmediate16);
    }

    [Fact]
    public void ImmediateAndRegisterPatternsPreserveArchitecturalFieldOrder()
    {
        Assert.True(PpcInstructionPatterns.TryGetLis(0x3D80807Eu, out var lisDestination, out var high));
        Assert.Equal(12, lisDestination);
        Assert.Equal(0x807Eu, high);

        Assert.True(PpcInstructionPatterns.TryGetAddi(0x398CFFF0u, out var addiDestination, out var addiSource, out var addiImmediate));
        Assert.Equal(12, addiDestination);
        Assert.Equal(12, addiSource);
        Assert.Equal(-16, addiImmediate);

        Assert.True(PpcInstructionPatterns.TryGetOri(0x618C3064u, out var oriSource, out var oriDestination, out var oriImmediate));
        Assert.Equal(12, oriSource);
        Assert.Equal(12, oriDestination);
        Assert.Equal(0x3064u, oriImmediate);

        var oris = EncodeImmediate(25, source: 12, destination: 9, immediate: 0x1234);
        Assert.True(PpcInstructionPatterns.TryGetOris(oris, out var orisSource, out var orisDestination, out var orisImmediate));
        Assert.Equal(12, orisSource);
        Assert.Equal(9, orisDestination);
        Assert.Equal(0x1234u, orisImmediate);

        var orWord = EncodeXForm(source: 12, destination: 9, otherSource: 12, extendedOpcode: 444);
        Assert.True(PpcInstructionPatterns.TryGetOr(orWord, out var source, out var destination, out var otherSource));
        Assert.Equal(12, source);
        Assert.Equal(9, destination);
        Assert.Equal(12, otherSource);

        var rlwinm = EncodeRlwinm(source: 3, destination: 4, shift: 0, maskBegin: 16, maskEnd: 31);
        Assert.True(PpcInstructionPatterns.TryGetRlwinm(rlwinm, out var rotateSource, out var rotateDestination, out var shift, out var maskBegin, out var maskEnd));
        Assert.Equal(3, rotateSource);
        Assert.Equal(4, rotateDestination);
        Assert.Equal(0, shift);
        Assert.Equal(16, maskBegin);
        Assert.Equal(31, maskEnd);
    }

    [Fact]
    public void SprPatternsRecognizeOnlyTheRequestedSpecialRegister()
    {
        Assert.True(PpcInstructionPatterns.TryGetMtspr(0x7D8903A6u, 9, out var ctrSource));
        Assert.Equal(12, ctrSource);
        Assert.False(PpcInstructionPatterns.TryGetMtspr(0x7D8903A6u, 8, out _));

        Assert.True(PpcInstructionPatterns.TryGetMtspr(0x7C0803A6u, 8, out var lrSource));
        Assert.Equal(0, lrSource);
    }

    [Fact]
    public void BranchPatternsPreserveLinkAbsoluteAndConditionalDistinctions()
    {
        const uint address = 0x80000100u;

        Assert.True(PpcControlFlow.IsRelativeUnlinkedBranch(0x48000008u));
        Assert.True(PpcControlFlow.TryDecodeRelativeBranchTarget(address, 0x48000008u, out var forward));
        Assert.Equal(0x80000108u, forward);

        Assert.True(PpcControlFlow.IsRelativeLinkedBranch(0x48000001u));
        Assert.True(PpcControlFlow.TryDecodeRelativeBranchLinkTarget(address, 0x48000001u, out var linked));
        Assert.Equal(address, linked);

        Assert.False(PpcControlFlow.IsRelativeUnlinkedBranch(0x48000002u)); // absolute b
        Assert.False(PpcControlFlow.TryDecodeRelativeBranchTarget(address, 0x48000001u, out _)); // bl

        Assert.True(PpcControlFlow.TryDecodeConditionalRelativeBranchTarget(address, 0x4182FFFCu, out var conditional));
        Assert.Equal(0x800000FCu, conditional);
        Assert.True(PpcControlFlow.MayChangeControlFlow(0x4182FFFCu));
        Assert.True(PpcControlFlow.MayChangeControlFlow(0x48000000u));
        Assert.True(PpcControlFlow.MayChangeControlFlow(0x4E800020u));
        Assert.False(PpcControlFlow.MayChangeControlFlow(0x398CFFF0u));
    }

    [Fact]
    public void ConservativeWritePolicyUsesTheArchitecturalDestinationRegister()
    {
        // ori RA, RS, UIMM writes RA. Two of the three scanners used to key this
        // on RS, so they evicted the source and kept a stale value in the
        // register the instruction actually overwrote.
        var ori = EncodeImmediate(24, source: 3, destination: 4, immediate: 1);
        Assert.True(PpcRegisterEffects.MayWriteGpr(ori, 4));
        Assert.False(PpcRegisterEffects.MayWriteGpr(ori, 3));

        // addi RT, RA, SIMM writes RT, which this encoder puts in the source field.
        var addi = EncodeImmediate(14, source: 5, destination: 6, immediate: 8);
        Assert.True(PpcRegisterEffects.MayWriteGpr(addi, 5));
        Assert.False(PpcRegisterEffects.MayWriteGpr(addi, 6));

        // lwzu RT, d(RA) writes both RT and the RA base.
        var lwzu = EncodeImmediate(33, source: 7, destination: 8, immediate: 4);
        Assert.True(PpcRegisterEffects.MayWriteGpr(lwzu, 7));
        Assert.True(PpcRegisterEffects.MayWriteGpr(lwzu, 8));

        // stwu RS, d(RA) writes the RA base back; stw writes no register.
        var stwu = EncodeImmediate(37, source: 1, destination: 1, immediate: 0xFFF0);
        Assert.True(PpcRegisterEffects.MayWriteGpr(stwu, 1));
        var stw = EncodeImmediate(36, source: 9, destination: 10, immediate: 0);
        Assert.False(PpcRegisterEffects.MayWriteGpr(stw, 9));
        Assert.False(PpcRegisterEffects.MayWriteGpr(stw, 10));

        // lfd writes an FPR, so no GPR dies; lfdu still writes its RA base.
        var lfd = EncodeImmediate(50, source: 11, destination: 12, immediate: 0);
        Assert.False(PpcRegisterEffects.MayWriteGpr(lfd, 11));
        Assert.False(PpcRegisterEffects.MayWriteGpr(lfd, 12));
        var lfdu = EncodeImmediate(51, source: 11, destination: 12, immediate: 8);
        Assert.True(PpcRegisterEffects.MayWriteGpr(lfdu, 12));

        // cmpi writes a CR field, not a GPR.
        var cmpi = EncodeImmediate(11, source: 0, destination: 13, immediate: 0);
        Assert.False(PpcRegisterEffects.MayWriteGpr(cmpi, 0));
        Assert.False(PpcRegisterEffects.MayWriteGpr(cmpi, 13));

        // X-form is treated as clobbering either candidate: `add r5,r5,r6` and
        // `or r5,r6,r6` share a primary opcode but not a destination field.
        var xform = EncodeImmediate(31, source: 14, destination: 15, immediate: 0);
        Assert.True(PpcRegisterEffects.MayWriteGpr(xform, 14));
        Assert.True(PpcRegisterEffects.MayWriteGpr(xform, 15));
    }

    private static uint EncodeImmediate(int opcode, int source, int destination, int immediate) =>
        ((uint)opcode << 26) |
        ((uint)source << 21) |
        ((uint)destination << 16) |
        (uint)(immediate & 0xFFFF);

    private static uint EncodeXForm(int source, int destination, int otherSource, int extendedOpcode) =>
        (31u << 26) |
        ((uint)source << 21) |
        ((uint)destination << 16) |
        ((uint)otherSource << 11) |
        ((uint)extendedOpcode << 1);

    private static uint EncodeRlwinm(int source, int destination, int shift, int maskBegin, int maskEnd) =>
        (21u << 26) |
        ((uint)source << 21) |
        ((uint)destination << 16) |
        ((uint)shift << 11) |
        ((uint)maskBegin << 6) |
        ((uint)maskEnd << 1);
}
