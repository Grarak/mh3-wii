using System.Collections.Generic;

namespace Translator.Core.Ir;

public abstract record IrNode(string Kind);

public abstract record IrInstruction(string Kind) : IrNode(Kind);

public sealed record IrAssign(string Destination, IrValue Value) : IrInstruction("assign");

public sealed record IrBinary(string Destination, IrValue Left, IrValue Right, string Op) : IrInstruction("binary");

public sealed record IrLoad(string Destination, IrAddress Address, int SizeBytes) : IrInstruction("load");

public sealed record IrStore(IrAddress Address, IrValue Source, int SizeBytes) : IrInstruction("store");

/// <summary>
/// Proves that [Base + MinOffset, Base + MinOffset + Length) is a contiguous
/// mapped guest-RAM range and materializes its host pointer. A null host value
/// records a failed proof and makes resolved accesses use their exact slow path.
/// </summary>
public sealed record IrResolveGuestMemoryRange(
    string Destination,
    IrValue Base,
    int MinOffset,
    int Length,
    bool NeedsReadAccess,
    bool NeedsWriteAccess) : IrInstruction("resolve_guest_memory_range");

public sealed record IrResolvedLoad(
    string Destination,
    string Range,
    IrAddress OriginalAddress,
    int RangeOffset,
    int SizeBytes) : IrInstruction("resolved_load");

public sealed record IrResolvedStore(
    string Range,
    IrAddress OriginalAddress,
    int RangeOffset,
    IrValue Source,
    int SizeBytes) : IrInstruction("resolved_store");

public sealed record IrResolvedPsqLoad(
    string Destination, string Range, IrValue OriginalAddress, int RangeOffset,
    uint W, uint I, uint? KnownGqr, bool GuardKnownGqr = false) : IrInstruction("resolved_psq_load");

public sealed record IrResolvedPsqStore(
    string Range, IrValue OriginalAddress, int RangeOffset, IrValue Source,
    uint W, uint I, uint? KnownGqr, bool GuardKnownGqr = false) : IrInstruction("resolved_psq_store");

public sealed record IrResolvedLoadPair(
    string FirstDestination, string SecondDestination, string Range,
    IrAddress FirstOriginalAddress, IrAddress SecondOriginalAddress,
    int RangeOffset, int ElementSizeBytes, bool Descending = false) : IrInstruction("resolved_load_pair");

public sealed record IrResolvedStorePair(
    string Range, IrAddress FirstOriginalAddress, IrAddress SecondOriginalAddress,
    int RangeOffset, IrValue FirstSource, IrValue SecondSource, int ElementSizeBytes,
    bool Descending = false)
    : IrInstruction("resolved_store_pair");

public sealed record IrCall(string Destination, string Target, IReadOnlyList<IrValue> Arguments) : IrInstruction("call");

/// <summary>
/// Indirect call through a register or computed address (e.g., blrl/bctrl).
/// Destination captures the link register write or return value slot, mirroring IrCall semantics.
/// </summary>
public sealed record IrIndirectCall(string Destination, IrValue Target, IReadOnlyList<IrValue> Arguments) : IrInstruction("indirect_call");

public sealed record IrSetCrField(int FieldIndex, IrValue Left, IrValue Right, bool IsUnsigned) : IrInstruction("set_cr_field");

/// <summary>
/// SSA merge of values coming from predecessor blocks.
/// Sources map predecessor label -> SSA value name.
/// </summary>
public sealed record IrPhi(string Destination, IReadOnlyDictionary<string, string> Sources) : IrInstruction("phi");

public sealed record IrBranch(string Condition, string TrueLabel, string FalseLabel, string ConditionRegister = "cr0") : IrInstruction("branch");

public sealed record IrJump(string TargetLabel) : IrInstruction("jump");

/// <summary>
/// Indirect jump through a register (e.g., bctr without link).
/// Unlike IrIndirectCall, this represents an intra-function jump (e.g., switch statement)
/// and should not perform a function call.
/// </summary>
public sealed record IrIndirectJump(IrValue Target) : IrInstruction("indirect_jump");

/// <summary>
/// Jump table produced from a recognized switch statement. Selector is the register
/// holding the computed target address, and each case maps a literal address to a
/// basic block label within the same function.
/// </summary>
public sealed record IrJumpTable(string Selector, IReadOnlyList<IrJumpTableCase> Cases) : IrInstruction("jump_table");

public sealed record IrJumpTableCase(uint TargetAddress, string TargetLabel);

public sealed record IrReturn(IrValue? Value) : IrInstruction("return");

public sealed record IrComment(string Text) : IrInstruction("comment");

public sealed record IrTracePpc(uint Address, string Disassembly, string RawHex)
    : IrInstruction("trace_ppc");

/// <summary>
/// Marks an instruction as intentionally untranslatable in the current runtime model.
/// Codegen should emit a runtime UNDEFINED trap that reports PC + raw opcode.
/// </summary>
public sealed record IrUndefined(uint Address, uint RawInstruction, string Disassembly, string? Reason = null)
    : IrInstruction("undefined");

public sealed record IrAddress(string Base, int Offset);

public sealed record IrValue(string Kind, string? RegisterName = null, long? Constant = null)
{
    public static IrValue Register(string name) => new("register", RegisterName: name);
    public static IrValue Imm(long value) => new("const", Constant: value);
}

public sealed record IrBasicBlock(string Label, IReadOnlyList<IrInstruction> Instructions);

public sealed record IrFunction(string Name, string EntryLabel, IReadOnlyList<IrBasicBlock> Blocks);
