using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Loading;

namespace Translator.Core.Disassembly;

internal static class JumpTableDetector
{
    private const int MaxBacktrackInstructions = 192;
    // MH3's item dispatch at 0x80229FDC has 635 entries. Keep a bounded
    // scan (16 KiB), while accepting larger compiler-generated switches.
    private const int MaxEntryCount = 4096;

    public static bool TryRecognize(
        IReadOnlyList<PpcInstruction> ordered,
        IReadOnlyDictionary<uint, int> indexByAddress,
        uint bctrAddress,
        uint functionStart,
        uint functionEnd,
        ProgramImage image,
        out IReadOnlyList<uint> targets)
    {
        targets = Array.Empty<uint>();
        if (!indexByAddress.TryGetValue(bctrAddress, out var bctrIndex))
        {
            return false;
        }

        var mtctrIndex = FindPrevious(ordered, bctrIndex, "mtctr", limit: 6);
        if (mtctrIndex < 0)
        {
            return false;
        }

        if (ordered[mtctrIndex].Operands.Count == 0 || ordered[mtctrIndex].Operands[0] is not PpcRegisterOperand ctrOperand)
        {
            return false;
        }
        var targetRegister = ctrOperand.Name;

        var loadIndex = FindPreviousLoad(ordered, mtctrIndex, targetRegister);
        if (loadIndex < 0)
        {
            return false;
        }

        var load = ordered[loadIndex];
        if (!string.Equals(load.Mnemonic, "lwzx", StringComparison.OrdinalIgnoreCase))
        {
            return false; // Only handle lwzx-based tables for now.
        }

        if (load.Operands.Count < 3 || load.Operands[0] is not PpcRegisterOperand dest ||
            load.Operands[1] is not PpcRegisterOperand baseReg ||
            load.Operands[2] is not PpcRegisterOperand indexReg)
        {
            return false;
        }

        if (!string.Equals(dest.Name, targetRegister, StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        var entriesAreRelative = false;
        if (!TryResolveLisAddiConstant(ordered, loadIndex, baseReg.Name, out var tableBase) &&
            !TryResolvePcRelativeLoadedConstant(ordered, loadIndex, baseReg.Name, image, out tableBase) &&
            !TryResolveLongLivedTableBase(ordered, indexByAddress, loadIndex, baseReg.Name, functionStart, out tableBase))
        {
            return false;
        }
        entriesAreRelative = HasRelativeEntryAdd(ordered, loadIndex, mtctrIndex, targetRegister, baseReg.Name);

        if (!TryFindUpperBound(ordered, loadIndex, indexReg.Name, out var upperBound) || upperBound < 0 || upperBound >= MaxEntryCount)
        {
            return false;
        }

        var entryCount = upperBound + 1;
        var dedupList = new List<uint>();
        var seen = new HashSet<uint>();
        for (var i = 0; i < entryCount; i++)
        {
            var entryAddress = unchecked(tableBase + (uint)(i * 4));
            if (!TryReadWord(image, entryAddress, out var entry))
            {
                return false;
            }
            var target = entriesAreRelative ? unchecked(tableBase + entry) : entry;

            if (!indexByAddress.ContainsKey(target))
            {
                // Allow targets that haven't been decoded yet as long as they
                // fall within the current function's decoding window.
                if (target < functionStart || target >= functionEnd || (target & 3) != 0)
                {
                    return false;
                }
            }

            if (seen.Add(target))
            {
                dedupList.Add(target);
            }
        }

        if (dedupList.Count == 0)
        {
            return false; // Not a real switch.
        }

        targets = dedupList;
        return true;
    }

    private static int FindPrevious(IReadOnlyList<PpcInstruction> ordered, int startIndex, string mnemonic, int limit)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= limit; i--)
        {
            // Comparison already ignores case, so lowering the needle only
            // allocated a copy of it.
            if (string.Equals(ordered[i].Mnemonic, mnemonic, StringComparison.OrdinalIgnoreCase))
            {
                return i;
            }
        }
        return -1;
    }

    private static bool HasRelativeEntryAdd(
        IReadOnlyList<PpcInstruction> ordered,
        int loadIndex,
        int mtctrIndex,
        string targetRegister,
        string tableBaseRegister)
    {
        for (var i = loadIndex + 1; i < mtctrIndex; i++)
        {
            var ins = ordered[i];
            if (!string.Equals(ins.Mnemonic, "add", StringComparison.OrdinalIgnoreCase) ||
                ins.Operands.Count < 3 ||
                ins.Operands[0] is not PpcRegisterOperand dest ||
                ins.Operands[1] is not PpcRegisterOperand left ||
                ins.Operands[2] is not PpcRegisterOperand right ||
                !RegistersEqual(dest.Name, targetRegister))
            {
                continue;
            }

            if ((RegistersEqual(left.Name, targetRegister) && RegistersEqual(right.Name, tableBaseRegister)) ||
                (RegistersEqual(right.Name, targetRegister) && RegistersEqual(left.Name, tableBaseRegister)))
            {
                return true;
            }
        }

        return false;
    }

    private static int FindPreviousLoad(IReadOnlyList<PpcInstruction> ordered, int startIndex, string register)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (ins.Operands.Count == 0 || ins.Operands[0] is not PpcRegisterOperand dest)
            {
                continue;
            }

            if (!string.Equals(dest.Name, register, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            // Decoder mnemonics are lowercase by construction.
            if (ins.Mnemonic is "lwzx" or "lwz")
            {
                return i;
            }
        }

        return -1;
    }

    private static bool TryResolveLisAddiConstant(IReadOnlyList<PpcInstruction> ordered, int startIndex, string register, out uint value)
    {
        var haveOffset = false;
        int offset = 0;
        var baseRegister = register;
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (ins.Operands.Count == 0 || ins.Operands[0] is not PpcRegisterOperand dest)
            {
                continue;
            }

            if (!string.Equals(dest.Name, baseRegister, StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            var mnemonic = ins.Mnemonic;
            if (!haveOffset && mnemonic == "addi" && ins.Operands.Count >= 3 && ins.Operands[1] is PpcRegisterOperand addBase &&
                ins.Operands[2] is PpcImmediateOperand addImm)
            {
                offset = (short)addImm.Value;
                haveOffset = true;
                // Track the source register so we can resolve lis/addi from a different base.
                baseRegister = addBase.Name;
                continue;
            }

            if (mnemonic == "lis" && ins.Operands.Count >= 2 && ins.Operands[1] is PpcImmediateOperand hiImm)
            {
                var hi = hiImm.Value << 16;
                var low = haveOffset ? offset : 0;
                value = unchecked((uint)(hi + low));
                return true;
            }
        }

        value = 0;
        return false;
    }

    private static bool TryResolveLongLivedTableBase(
        IReadOnlyList<PpcInstruction> ordered, IReadOnlyDictionary<uint, int> indexByAddress,
        int loadIndex, string register, uint functionStart, out uint value)
    {
        value = 0;
        // An invariant table-page register can be initialized well before the
        // bounded local switch pattern (for example outside a rendering loop).
        // Only extend the search for a nonvolatile GPR, and prove its definition
        // along the decoded control-flow paths instead of guessing an address.
        for (var addIndex = loadIndex - 1; addIndex >= 0 && loadIndex - addIndex <= 6; --addIndex)
        {
            var add = ordered[addIndex];
            if (!WritesGpr(add, register)) continue;
            if (add.Mnemonic != "addi" || add.Operands.Count != 3 ||
                add.Operands[1] is not PpcRegisterOperand source ||
                add.Operands[2] is not PpcImmediateOperand low ||
                source.Name.Length < 2 || !int.TryParse(source.Name.AsSpan(1), out var number) ||
                number < 14 || number > 31) return false;
            for (var definition = addIndex - 1; definition >= 0; --definition)
            {
                var high = ordered[definition];
                if (high.Mnemonic != "lis" || high.Operands.Count != 2 ||
                    high.Operands[0] is not PpcRegisterOperand dest || !RegistersEqual(dest.Name, source.Name) ||
                    high.Operands[1] is not PpcImmediateOperand immediate) continue;
                if (!DefinitionReachesEveryPath(ordered, indexByAddress, functionStart,
                        high.Address, add.Address, source.Name)) return false;
                value = unchecked((uint)((immediate.Value << 16) + (short)low.Value));
                return true;
            }
            return false;
        }
        return false;
    }

    private static bool DefinitionReachesEveryPath(
        IReadOnlyList<PpcInstruction> ordered, IReadOnlyDictionary<uint, int> indexByAddress,
        uint start, uint definition, uint use, string register)
    {
        var pending = new Queue<(uint Address, bool Defined)>();
        var visited = new HashSet<(uint Address, bool Defined)>();
        pending.Enqueue((start, false));
        var reached = false;
        while (pending.TryDequeue(out var state))
        {
            if (!visited.Add(state) || !indexByAddress.TryGetValue(state.Address, out var index)) continue;
            var ins = ordered[index];
            if (ins.Address == use)
            {
                if (!state.Defined) return false;
                reached = true;
            }
            var defined = ins.Address == definition || (state.Defined && !WritesGpr(ins, register));
            if (!ins.IsCall)
                foreach (var target in ins.BranchTargets) pending.Enqueue((target, defined));
            if (ins.IsCall || (!ins.IsReturn && ins.Mnemonic != "bctr" &&
                              (ins.BranchTargets.Count == 0 || ins.IsConditionalBranch)))
                pending.Enqueue((ins.EndAddress, defined));
        }
        return reached;
    }

    private static bool WritesGpr(PpcInstruction ins, string register)
    {
        // Calls preserve r14-r31 under the PPC ABI. This helper is used for
        // nonvolatile dataflow, and for the short addi-to-lwzx definition chain.
        if (ins.IsCall) return register is "r0" or "r3" or "r4" or "r5" or "r6" or
            "r7" or "r8" or "r9" or "r10" or "r11" or "r12";
        if (ins.Mnemonic is "lswi" or "lswx" or "lscbx") return true;
        if (ins.Mnemonic.EndsWith("u", StringComparison.Ordinal) ||
            ins.Mnemonic.EndsWith("ux", StringComparison.Ordinal))
        {
            if (ins.Operands.Count > 1 && ins.Operands[1] is PpcDisplacementOperand displacement &&
                RegistersEqual(displacement.BaseRegister, register)) return true;
            if (ins.Operands.Count > 2 && ins.Operands[1] is PpcRegisterOperand indexedBase &&
                RegistersEqual(indexedBase.Name, register)) return true;
        }
        if (ins.Mnemonic == "lmw" && ins.Operands.Count > 0 && ins.Operands[0] is PpcRegisterOperand first &&
            int.TryParse(first.Name.AsSpan(1), out var firstNumber) &&
            int.TryParse(register.AsSpan(1), out var number) && number >= firstNumber) return true;
        if (ins.Mnemonic.StartsWith("st", StringComparison.Ordinal) ||
            ins.Mnemonic.StartsWith("cmp", StringComparison.Ordinal) ||
            ins.Mnemonic.StartsWith("mt", StringComparison.Ordinal) ||
            ins.Mnemonic.StartsWith("dc", StringComparison.Ordinal) ||
            ins.Mnemonic.StartsWith("ic", StringComparison.Ordinal)) return false;
        return ins.Operands.Count > 0 && ins.Operands[0] is PpcRegisterOperand destination &&
            RegistersEqual(destination.Name, register);
    }

    private static bool TryResolvePcRelativeLoadedConstant(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string register,
        ProgramImage image,
        out uint value)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (!string.Equals(ins.Mnemonic, "lwz", StringComparison.OrdinalIgnoreCase) ||
                ins.Operands.Count < 2 ||
                ins.Operands[0] is not PpcRegisterOperand dest ||
                ins.Operands[1] is not PpcDisplacementOperand displacement ||
                !RegistersEqual(dest.Name, register))
            {
                continue;
            }

            if (!TryResolvePicBaseRegister(ordered, i, displacement.BaseRegister, image, out var picBase))
            {
                continue;
            }

            var literalAddress = AddSigned(picBase, displacement.Offset);
            if (TryReadWord(image, literalAddress, out value))
            {
                return true;
            }
        }

        value = 0;
        return false;
    }

    private static bool TryResolvePicBaseRegister(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string register,
        ProgramImage image,
        out uint value)
    {
        for (var addIndex = startIndex - 1; addIndex >= 0 && startIndex - addIndex <= MaxBacktrackInstructions; addIndex--)
        {
            var add = ordered[addIndex];
            if (!string.Equals(add.Mnemonic, "add", StringComparison.OrdinalIgnoreCase) ||
                add.Operands.Count < 3 ||
                add.Operands[0] is not PpcRegisterOperand dest ||
                add.Operands[1] is not PpcRegisterOperand left ||
                add.Operands[2] is not PpcRegisterOperand right ||
                !RegistersEqual(dest.Name, register))
            {
                continue;
            }

            string offsetRegister;
            if (RegistersEqual(left.Name, register))
            {
                offsetRegister = right.Name;
            }
            else if (RegistersEqual(right.Name, register))
            {
                offsetRegister = left.Name;
            }
            else
            {
                continue;
            }

            if (!TryFindRegisterDisplacementLoadBefore(ordered, addIndex, offsetRegister, register, out var offsetLoad) ||
                !TryFindMflrBefore(ordered, offsetLoad.Index, register, out var mflrIndex) ||
                !TryFindLinkCallBefore(ordered, mflrIndex, out var linkAddress))
            {
                continue;
            }

            var offsetAddress = AddSigned(linkAddress, offsetLoad.Displacement);
            if (!TryReadWord(image, offsetAddress, out var picOffset))
            {
                continue;
            }

            value = unchecked(linkAddress + picOffset);
            return true;
        }

        value = 0;
        return false;
    }

    private static bool TryFindRegisterDisplacementLoadBefore(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string destinationRegister,
        string baseRegister,
        out (int Index, int Displacement) result)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (string.Equals(ins.Mnemonic, "lwz", StringComparison.OrdinalIgnoreCase) &&
                ins.Operands.Count >= 2 &&
                ins.Operands[0] is PpcRegisterOperand dest &&
                ins.Operands[1] is PpcDisplacementOperand displacement &&
                RegistersEqual(dest.Name, destinationRegister) &&
                RegistersEqual(displacement.BaseRegister, baseRegister))
            {
                result = (i, displacement.Offset);
                return true;
            }
        }

        result = default;
        return false;
    }

    private static bool TryFindMflrBefore(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        string destinationRegister,
        out int mflrIndex)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (string.Equals(ins.Mnemonic, "mflr", StringComparison.OrdinalIgnoreCase) &&
                ins.Operands.Count >= 1 &&
                ins.Operands[0] is PpcRegisterOperand dest &&
                RegistersEqual(dest.Name, destinationRegister))
            {
                mflrIndex = i;
                return true;
            }
        }

        mflrIndex = -1;
        return false;
    }

    private static bool TryFindLinkCallBefore(
        IReadOnlyList<PpcInstruction> ordered,
        int startIndex,
        out uint linkAddress)
    {
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (ins.IsCall)
            {
                linkAddress = ins.EndAddress;
                return true;
            }
        }

        linkAddress = 0;
        return false;
    }

    private static bool TryFindUpperBound(IReadOnlyList<PpcInstruction> ordered, int startIndex, string register, out int upperBound)
    {
        // Compilers commonly compare the logical selector, then shift it into
        // a different register for lwzx. Follow that definition before looking
        // for comparisons: an older comparison of the destination register
        // describes a different value (MH3 VIInit used r0 for both purposes).
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (ins.Operands.Count == 0 || ins.Operands[0] is not PpcRegisterOperand dest ||
                !RegistersEqual(dest.Name, register)) continue;
            if (ins.Mnemonic is "cmpwi" or "cmplwi" or "stw" or "stwu" or "sth" or "stb") continue;
            var wordScale = ins.Mnemonic == "slwi" && ins.Operands.Count == 3 &&
                ins.Operands[2] is PpcImmediateOperand { Value: 2 };
            wordScale |= ins.Mnemonic == "rlwinm" && ins.Operands.Count == 5 &&
                ins.Operands[2] is PpcImmediateOperand { Value: 2 } &&
                ins.Operands[3] is PpcImmediateOperand { Value: 0 } &&
                ins.Operands[4] is PpcImmediateOperand { Value: 29 };
            if (wordScale && ins.Operands[1] is PpcRegisterOperand source)
            {
                register = source.Name;
                startIndex = i;
            }
            break;
        }
        // Preferred: compare directly on the same register used for lwzx indexing.
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (ins.Operands.Count < 2 || ins.Operands[0] is not PpcRegisterOperand reg ||
                !string.Equals(reg.Name, register, StringComparison.OrdinalIgnoreCase) ||
                ins.Operands[1] is not PpcImmediateOperand imm)
            {
                continue;
            }

            if (ins.Mnemonic is "cmplwi" or "cmpwi")
            {
                upperBound = imm.Value;
                return true;
            }
        }

        // Fallback: some loops compare a logical counter (e.g. r18) distinct from the byte-offset
        // register (e.g. r30) that feeds lwzx, so the bound check may not target the index register.
        for (var i = startIndex - 1; i >= 0 && startIndex - i <= MaxBacktrackInstructions; i--)
        {
            var ins = ordered[i];
            if (ins.Operands.Count < 2 || ins.Operands[1] is not PpcImmediateOperand imm)
            {
                continue;
            }

            if (ins.Mnemonic is not ("cmplwi" or "cmpwi"))
            {
                continue;
            }

            if (imm.Value < 0 || imm.Value >= MaxEntryCount)
            {
                continue;
            }

            upperBound = imm.Value;
            return true;
        }

        upperBound = 0;
        return false;
    }

    private static bool TryReadWord(ProgramImage image, uint address, out uint value)
    {
        if (!image.Contains(address, sizeof(uint)))
        {
            value = 0;
            return false;
        }

        var span = image.Memory.AsSpan(image.GetOffset(address, sizeof(uint)), sizeof(uint));
        value = BinaryPrimitives.ReadUInt32BigEndian(span);
        return true;
    }

    private static bool RegistersEqual(string left, string right) =>
        string.Equals(left, right, StringComparison.OrdinalIgnoreCase);

    private static uint AddSigned(uint value, int offset) =>
        unchecked(value + (uint)offset);

}
