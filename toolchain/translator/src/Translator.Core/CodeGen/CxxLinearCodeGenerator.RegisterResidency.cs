using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Translator.Core.Analysis;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// Guest register residency for one emitted function body: C++ locals are authoritative between call boundaries (host regs), synced via flush/reload at each one.
    /// Emitted twice, a recording pass then the real pass, so any register reference outside the recorded set fails loud instead of silently reading stale <c>ctx-&gt;</c> state.
    /// </summary>
    private sealed class RegisterResidency
    {
        private readonly bool _recording;
        private readonly SortedSet<int> _gprs = new();
        private readonly SortedSet<int> _fprs = new();
        private readonly SortedSet<int> _writtenGprs = new();
        private readonly SortedSet<int> _writtenFprs = new();
        private bool _cr;
        private bool _ctr;
        private bool _xer;
        private bool _writesCr;
        private bool _writesCtr;
        private bool _writesXer;
        // r14-r31 some call boundary (not an exit) publishes to CpuContext; recorded by the
        // discovery pass. See DirtiedPreservedGprMask.
        private uint _boundaryFlushGprMask;

        private RegisterResidency(bool recording) => _recording = recording;

        public static RegisterResidency CreateRecorder() => new(true);

        public RegisterResidency Snapshot()
        {
            var snapshot = new RegisterResidency(false);
            snapshot._gprs.UnionWith(_gprs);
            snapshot._fprs.UnionWith(_fprs);
            snapshot._writtenGprs.UnionWith(_writtenGprs);
            snapshot._writtenFprs.UnionWith(_writtenFprs);
            snapshot._cr = _cr;
            snapshot._ctr = _ctr;
            snapshot._xer = _xer;
            snapshot._writesCr = _writesCr;
            snapshot._writesCtr = _writesCtr;
            snapshot._writesXer = _writesXer;
            snapshot._boundaryFlushGprMask = _boundaryFlushGprMask;
            return snapshot;
        }

        public bool IsEmpty => _gprs.Count == 0 && _fprs.Count == 0 && !_cr && !_ctr && !_xer;

        /// <summary>
        /// r14-r31 this body preserves for its caller (its own contract's
        /// <see cref="GuestAbiContract.GprPreservedMask"/>, which callers rely on). Exits
        /// don't publish them: no caller reads them back from CpuContext.
        /// </summary>
        public uint ExitPreservedGprMask { get; set; }

        /// <summary>
        /// Preserved r14-r31 this body changes and publishes to CpuContext at a call boundary
        /// (for a callee that reads or may change it). The caller relies on CpuContext holding
        /// its value afterwards whenever the register only passes through it, from a caller
        /// that syncs it to a deeper callee that may change it; so every exit stores the entry
        /// value back, as the guest's own restore would.
        /// </summary>
        private uint DirtiedPreservedGprMask
        {
            get
            {
                var mask = ExitPreservedGprMask & _boundaryFlushGprMask & NonvolatileGprPreservationAnalyzer.NonvolatileGprMask;
                var written = 0u;
                foreach (var gpr in _writtenGprs) written |= 1u << gpr;
                return mask & written;
            }
        }

        private static string GprEntryLocal(int index) => GprLocal(index) + "_entry";

        private void AppendEntryRestores(StringBuilder sb, string pad)
        {
            var dirtied = DirtiedPreservedGprMask;
            for (var gpr = 14; gpr < 32; ++gpr)
                if ((dirtied & (1u << gpr)) != 0)
                    sb.Append(pad).Append("ctx->gpr[").Append(gpr).Append("] = ").Append(GprEntryLocal(gpr)).AppendLine(";");
        }

        /// <summary>
        /// Set when this body is a register-argument variant: its argument registers start from
        /// the C parameters, and its exits leave the returned registers (and arguments the
        /// function never changes) out of CpuContext.
        /// </summary>
        public RegArgsSignature? RegArgsEntry { get; private set; }
        private uint _exitExcludedGprMask;
        private uint _exitExcludedFprMask;

        /// <summary>
        /// Turns this body into <paramref name="signature"/>'s variant. The argument registers are
        /// marked written: the caller never stored them, so every boundary that syncs them must.
        /// </summary>
        public void EnterThroughRegArgs(RegArgsSignature signature, GuestAbiContract contract)
        {
            RegArgsEntry = signature;
            foreach (var gpr in signature.ArgGprs)
                if (_gprs.Contains(gpr)) _writtenGprs.Add(gpr);
            foreach (var fpr in signature.ArgFprs)
                if (_fprs.Contains(fpr)) _writtenFprs.Add(fpr);
            _exitExcludedGprMask = signature.RetGprMask | (signature.ArgGprMask & ~contract.GprPossibleWriteMask);
            _exitExcludedFprMask = signature.RetFprMask | (signature.ArgFprMask & ~contract.FprPossibleWriteMask);
        }

        public bool HasGpr(int index) => _gprs.Contains(index);
        public bool HasFpr(int index) => _fprs.Contains(index);

        public static string GprLocal(int index) => GprRegisterName(index);
        public static string FprLocal(int index) => FprRegisterName(index);
        public const string CrLocal = "cr";
        public const string CtrLocal = "ctr";
        public const string XerLocal = "xer";

        public IEnumerable<string> DeclaredLocalNames
        {
            get
            {
                foreach (var gpr in _gprs) yield return GprLocal(gpr);
                foreach (var fpr in _fprs) yield return FprLocal(fpr);
                if (_cr) yield return CrLocal;
                if (_ctr) yield return CtrLocal;
                if (_xer) yield return XerLocal;
            }
        }

        public string Gpr(int index, bool written)
        {
            Track(_gprs, _writtenGprs, index, written, "r");
            return GprLocal(index);
        }

        /// <summary>Scalar (PS0) view of a localized FPR.</summary>
        public string FprScalar(int index, bool written)
        {
            Track(_fprs, _writtenFprs, index, written, "f");
            return FprScalarLocalName(index);
        }

        /// <summary>Whole <c>PPC_FPR</c> value of a localized FPR.</summary>
        public string FprStorage(int index, bool written)
        {
            Track(_fprs, _writtenFprs, index, written, "f");
            return FprLocal(index);
        }

        public string Cr(bool written)
        {
            if (_recording)
            {
                _cr = true;
                _writesCr |= written;
            }
            else if (!_cr || (written && !_writesCr))
            {
                throw new InvalidOperationException(
                    "Register residency did not localize cr for this function body.");
            }

            return CrLocal;
        }

        public string Ctr(bool written)
        {
            if (_recording)
            {
                _ctr = true;
                _writesCtr |= written;
            }
            else if (!_ctr || (written && !_writesCtr))
            {
                throw new InvalidOperationException(
                    "Register residency did not localize ctr for this function body.");
            }

            return CtrLocal;
        }

        /// <summary>
        /// XER, localized as one value (not just summary-overflow) because the carry helpers
        /// read/modify/write it and every integer compare reads XER.SO into its CR field.
        /// </summary>
        public string Xer(bool written)
        {
            if (_recording)
            {
                _xer = true;
                _writesXer |= written;
            }
            else if (!_xer || (written && !_writesXer))
            {
                throw new InvalidOperationException(
                    "Register residency did not localize xer for this function body.");
            }

            return XerLocal;
        }

        private void Track(SortedSet<int> referenced, SortedSet<int> written, int index, bool isWrite, string prefix)
        {
            if (_recording)
            {
                referenced.Add(index);
                if (isWrite) written.Add(index);
                return;
            }

            if (!referenced.Contains(index) || (isWrite && !written.Contains(index)))
            {
                throw new InvalidOperationException(
                    $"Register residency did not localize {prefix}{index} for this function body.");
            }
        }

        /// <summary>
        /// Entry loads. Every referenced register is loaded unconditionally:
        /// liveness-narrowed initialization is a later optimization and reading
        /// an architectural register is always well defined.
        /// </summary>
        public void AppendEntryLoads(StringBuilder sb)
        {
            if (_recording) return;
            var argGprs = RegArgsEntry?.ArgGprMask ?? 0u;
            var argFprs = RegArgsEntry?.ArgFprMask ?? 0u;
            foreach (var gpr in _gprs)
            {
                if ((argGprs & (1u << gpr)) != 0)
                    sb.Append("    uint32_t ").Append(GprLocal(gpr)).Append(" = arg_r").Append(gpr).AppendLine(";");
                else
                    sb.Append("    uint32_t ").Append(GprLocal(gpr)).Append(" = ctx->gpr[").Append(gpr).AppendLine("];");
            }
            foreach (var fpr in _fprs)
            {
                if ((argFprs & (1u << fpr)) != 0)
                    sb.Append("    PPC_FPR ").Append(FprLocal(fpr)).Append("; ").Append(FprLocal(fpr)).Append(".d = arg_f").Append(fpr).AppendLine(";");
                else
                    sb.Append("    PPC_FPR ").Append(FprLocal(fpr)).Append(" = ctx->fpr[").Append(fpr).AppendLine("];");
            }
            // Arguments only callees read: they belong in the context from the start.
            foreach (var gpr in RegArgsEntry?.ArgGprs ?? Array.Empty<int>())
                if (!_gprs.Contains(gpr)) sb.Append("    ctx->gpr[").Append(gpr).Append("] = arg_r").Append(gpr).AppendLine(";");
            foreach (var fpr in RegArgsEntry?.ArgFprs ?? Array.Empty<int>())
                if (!_fprs.Contains(fpr)) sb.Append("    ctx->fpr[").Append(fpr).Append("].d = arg_f").Append(fpr).AppendLine(";");
            var dirtied = DirtiedPreservedGprMask;
            for (var gpr = 14; gpr < 32; ++gpr)
                if ((dirtied & (1u << gpr)) != 0)
                    sb.Append("    const uint32_t ").Append(GprEntryLocal(gpr)).Append(" = ").Append(GprLocal(gpr)).AppendLine(";");
            if (_cr) sb.AppendLine($"    uint32_t {CrLocal} = ctx->cr;");
            if (_ctr) sb.AppendLine($"    uint32_t {CtrLocal} = ctx->ctr;");
            if (_xer) sb.AppendLine($"    uint32_t {XerLocal} = ctx->xer;");
        }

        /// <summary>
        /// Stores the possibly-written localized registers back into
        /// <c>CpuContext</c>. Storing an unmodified register writes the same
        /// value back, so the conservative write set stays correct.
        /// </summary>
        public void AppendFlush(StringBuilder sb, string pad)
        {
            if (_recording) return;
            AppendFlushCore(sb, pad, ResidencyBoundarySync.Full.ExceptGprs(ExitPreservedGprMask));
            AppendEntryRestores(sb, pad);
        }

        /// <summary>
        /// Flush before a return to the caller. A register-argument variant hands its results
        /// back as the C return value instead.
        /// </summary>
        public void AppendExitFlush(StringBuilder sb, string pad)
        {
            if (_recording) return;
            AppendFlushCore(sb, pad, ResidencyBoundarySync.Full
                .ExceptGprs(ExitPreservedGprMask | _exitExcludedGprMask)
                .ExceptFprs(_exitExcludedFprMask));
            AppendEntryRestores(sb, pad);
        }

        /// <summary>
        /// Contract-narrowed flush. Only the registers the boundary can observe
        /// or overwrite are published; see <see cref="ResidencyBoundarySync"/>
        /// for why the write set has to participate in the flush mask.
        /// </summary>
        public void AppendFlush(StringBuilder sb, string pad, ResidencyBoundarySync sync)
        {
            if (_recording)
            {
                _boundaryFlushGprMask |= sync.FlushGprMask;
                return;
            }
            AppendFlushCore(sb, pad, sync);
        }

        private void AppendFlushCore(StringBuilder sb, string pad, ResidencyBoundarySync sync)
        {
            foreach (var gpr in _writtenGprs)
                if (sync.FlushesGpr(gpr))
                    sb.Append(pad).Append("ctx->gpr[").Append(gpr).Append("] = ").Append(GprLocal(gpr)).AppendLine(";");
            foreach (var fpr in _writtenFprs)
                if (sync.FlushesFpr(fpr))
                    sb.Append(pad).Append("ctx->fpr[").Append(fpr).Append("] = ").Append(FprLocal(fpr)).AppendLine(";");
            if (_writesCr && sync.FlushesCr) sb.Append(pad).AppendLine($"ctx->cr = {CrLocal};");
            if (_writesCtr && sync.FlushesCtr) sb.Append(pad).AppendLine($"ctx->ctr = {CtrLocal};");
            if (_writesXer && sync.FlushesXer) sb.Append(pad).AppendLine($"ctx->xer = {XerLocal};");
        }

        /// <summary>
        /// Reloads the full localized set after a boundary returns, unconditionally: a callee can
        /// change any volatile register and a nonvolatile one too if it restores it itself.
        /// </summary>
        public void AppendReload(StringBuilder sb, string pad) => AppendReload(sb, pad, ResidencyBoundarySync.Full);

        /// <summary>Contract-narrowed reload.</summary>
        public void AppendReload(StringBuilder sb, string pad, ResidencyBoundarySync sync)
        {
            if (_recording) return;
            foreach (var gpr in _gprs)
                if (sync.ReloadsGpr(gpr))
                    sb.Append(pad).Append(GprLocal(gpr)).Append(" = ctx->gpr[").Append(gpr).AppendLine("];");
            foreach (var fpr in _fprs)
                if (sync.ReloadsFpr(fpr))
                    sb.Append(pad).Append(FprLocal(fpr)).Append(" = ctx->fpr[").Append(fpr).AppendLine("];");
            if (_cr && sync.ReloadsCr) sb.Append(pad).AppendLine($"{CrLocal} = ctx->cr;");
            if (_ctr && sync.ReloadsCtr) sb.Append(pad).AppendLine($"{CtrLocal} = ctx->ctr;");
            if (_xer && sync.ReloadsXer) sb.Append(pad).AppendLine($"{XerLocal} = ctx->xer;");
        }
    }

    /// <summary>
    /// Which localized registers one call boundary must sync. Flush mask is <c>read-before-write | possible-write</c>, not just callee reads, or an unflushed possibly-written register reloads stale.
    /// Reload mask is the callee's possible-write mask (transitive union, not net effect); safe even for a spill/restore, which just re-reads a value already restored to what this frame flushed.
    /// Falls back to <see cref="Full"/> without a precise contract (missing, full-fence, indirect, or CpuContext-touching marshalling).
    /// </summary>
    internal sealed record ResidencyBoundarySync(
        uint FlushGprMask,
        uint FlushFprMask,
        bool FlushesCr,
        bool FlushesCtr,
        uint ReloadGprMask,
        uint ReloadFprMask,
        bool ReloadsCr,
        bool ReloadsCtr,
        bool FlushesXer = false,
        bool ReloadsXer = false)
    {
        public static ResidencyBoundarySync Full { get; } = new(
            uint.MaxValue, uint.MaxValue, true, true,
            uint.MaxValue, uint.MaxValue, true, true,
            FlushesXer: true, ReloadsXer: true);

        /// <summary>
        /// Fast arm of a state-free call site (explicit-state ABI, locals carry args/results) still flushes r1 and XER unconditionally, since a guest gather-pipe store can trigger GX HLE mid-call servicing a VI retrace or OS alarm whose handlers read them architecturally.
        /// Flush-only; the slow arm (<c>InvokeDirectCpu</c>) keeps a full <see cref="Full"/> pair.
        /// </summary>
        public static ResidencyBoundarySync StateFreeResidentOuter { get; } = new(
            FlushGprMask: 1u << 1,
            FlushFprMask: 0u,
            FlushesCr: false,
            FlushesCtr: false,
            ReloadGprMask: 0u,
            ReloadFprMask: 0u,
            ReloadsCr: false,
            ReloadsCtr: false,
            FlushesXer: true,
            ReloadsXer: false);

        /// <summary>
        /// Full minus the r14-r31 a callee preserves. Such a register must not be reloaded: the
        /// callee never published it, and anything it or its callees left in CpuContext is stale.
        /// A callee of unknown contract follows the EABI, so it preserves all of them.
        /// </summary>
        public static ResidencyBoundarySync FullForCallee(GuestAbiContract? callee) =>
            Full.ExceptGprs(callee?.GprPreservedMask ?? NonvolatileGprPreservationAnalyzer.NonvolatileGprMask);

        public ResidencyBoundarySync ExceptGprs(uint mask) => mask == 0 ? this : this with
        {
            FlushGprMask = FlushGprMask & ~mask,
            ReloadGprMask = ReloadGprMask & ~mask
        };

        public ResidencyBoundarySync ExceptFprs(uint mask) => mask == 0 ? this : this with
        {
            FlushFprMask = FlushFprMask & ~mask,
            ReloadFprMask = ReloadFprMask & ~mask
        };

        public bool FlushesGpr(int index) => (FlushGprMask & (1u << index)) != 0;
        public bool FlushesFpr(int index) => (FlushFprMask & (1u << index)) != 0;
        public bool ReloadsGpr(int index) => (ReloadGprMask & (1u << index)) != 0;
        public bool ReloadsFpr(int index) => (ReloadFprMask & (1u << index)) != 0;

        public bool IsFull =>
            FlushGprMask == uint.MaxValue && FlushFprMask == uint.MaxValue && FlushesCr && FlushesCtr &&
            ReloadGprMask == uint.MaxValue && ReloadFprMask == uint.MaxValue && ReloadsCr && ReloadsCtr &&
            FlushesXer && ReloadsXer;

        /// <summary>
        /// Adds a register class back to both masks, for call sites that emit their own architectural
        /// <c>CpuContext</c> traffic (paired-single return normalization) the callee contract omits.
        /// </summary>
        public ResidencyBoundarySync WithFpr(int index) => this with
        {
            FlushFprMask = FlushFprMask | (1u << index),
            ReloadFprMask = ReloadFprMask | (1u << index)
        };

        public bool IsEmpty =>
            FlushGprMask == 0 && FlushFprMask == 0 && !FlushesCr && !FlushesCtr &&
            ReloadGprMask == 0 && ReloadFprMask == 0 && !ReloadsCr && !ReloadsCtr &&
            !FlushesXer && !ReloadsXer;

        /// <summary>
        /// Boundary set for a non-guest helper's implicit <c>CpuContext</c> effects, from
        /// <see cref="Translator.Core.Analysis.GuestHelperEffectCatalog"/>. Explicit argument/destination registers
        /// need no sync; unknown helpers catalog as complete boundaries and arrive as full masks; a pure helper is empty.
        /// </summary>
        public static ResidencyBoundarySync FromHelperEffect(Translator.Core.Analysis.GuestHelperEffect effect) => new(
            FlushGprMask: effect.GprReadMask | effect.GprWriteMask,
            FlushFprMask: effect.FprReadMask | effect.FprWriteMask,
            FlushesCr: (effect.CrReadMask | effect.CrWriteMask) != 0,
            FlushesCtr: effect.ReadsCtr || effect.WritesCtr,
            ReloadGprMask: effect.GprWriteMask,
            ReloadFprMask: effect.FprWriteMask,
            ReloadsCr: effect.CrWriteMask != 0,
            ReloadsCtr: effect.WritesCtr,
            FlushesXer: effect.ReadsXer || effect.WritesXer,
            ReloadsXer: effect.WritesXer);

        /// <summary>
        /// r1 flushes unconditionally: typed native stubs derive their contract from a host signature and can't see the body re-enter guest code via HLE interrupt delivery, whose callback chains build frames from <c>ctx-&gt;gpr[1]</c>.
        /// A stale r1 once let such a callback overwrite a live frame's save area (return address landed in nw4r::ef's saved r29 at postVRetrace, 0x80210050, as a wild read). Flush-only; caller keeps its stack pointer.
        /// </summary>
        private const uint StackPointerFlushMask = 1u << 1;

        public static ResidencyBoundarySync FromCalleeContract(GuestAbiContract contract)
        {
            if (contract.HasFullSynchronizationFence) return FullForCallee(contract);
            return new ResidencyBoundarySync(
                FlushGprMask: contract.GprReadBeforeWriteMask | contract.GprPossibleWriteMask |
                              StackPointerFlushMask,
                FlushFprMask: contract.FprReadBeforeWriteMask | contract.FprPossibleWriteMask,
                // The contract tracks CR per field while the local is the whole
                // packed register, so any intersecting field forces the sync.
                FlushesCr: (contract.CrReadBeforeWriteMask | contract.CrPossibleWriteMask) != 0,
                FlushesCtr: contract.ReadsCtrBeforeWrite || contract.MayWriteCtr,
                ReloadGprMask: contract.GprPossibleWriteMask,
                ReloadFprMask: contract.FprPossibleWriteMask,
                ReloadsCr: contract.CrPossibleWriteMask != 0,
                ReloadsCtr: contract.MayWriteCtr,
                // Flushed unconditionally for the same reason as r1: interrupt handlers read XER.SO out
                // of the architectural copy. Reload stays contract-driven so a handler can't clobber it.
                FlushesXer: true,
                ReloadsXer: contract.MayWriteXer);
        }
    }

    /// <summary>
    /// Ambient residency for the body being emitted. Thread-local, not an explicit parameter, since threading one through 30+ static formatting helpers risks a missed path silently reading stale <c>ctx-&gt;gpr[N]</c>; also keeps parallel per-function emission independent.
    /// </summary>
    [ThreadStatic]
    private static RegisterResidency? _activeResidency;

    private static void AppendFlush(StringBuilder sb, string pad)
    {
        if (_activeResidency is { } residency) residency.AppendFlush(sb, pad);
    }

    private static void AppendExitFlush(StringBuilder sb, string pad)
    {
        if (_activeResidency is { } residency) residency.AppendExitFlush(sb, pad);
    }

    private static void AppendReload(StringBuilder sb, string pad)
    {
        if (_activeResidency is { } residency) residency.AppendReload(sb, pad);
    }

    private static void AppendFlush(StringBuilder sb, string pad, ResidencyBoundarySync sync)
    {
        if (_activeResidency is { } residency) residency.AppendFlush(sb, pad, sync);
    }

    private static void AppendReload(StringBuilder sb, string pad, ResidencyBoundarySync sync)
    {
        if (_activeResidency is { } residency) residency.AppendReload(sb, pad, sync);
    }

    /// <summary>
    /// Resolves the boundary sync set for a direct guest call, falling back to <see cref="ResidencyBoundarySync.Full"/> when narrowing is disabled, residency is inactive, the callee has no contract or a full-sync fence, or a mod could replace the callee with a re-translation this body's contract doesn't describe.
    /// </summary>
    private static ResidencyBoundarySync ResolveDirectCallBoundarySync(
        uint target,
        IReadOnlyDictionary<uint, GuestAbiContract> guestAbiContracts,
        IReadOnlySet<uint> modOverridableCallTargets)
    {
        if (_activeResidency is null ||
            modOverridableCallTargets.Contains(target) ||
            !guestAbiContracts.TryGetValue(target, out var contract))
        {
            return ResidencyBoundarySync.FullForCallee(null);
        }

        return ResidencyBoundarySync.FromCalleeContract(contract);
    }

    /// <summary>
    /// Removes an explicit helper destination register from a helper-effect
    /// reload mask. The destination local holds the helper's return value; the
    /// implicit-context reload must not overwrite it.
    /// </summary>
    private static ResidencyBoundarySync ExcludeDestinationFromReload(
        ResidencyBoundarySync sync,
        string? destination)
    {
        if (string.IsNullOrWhiteSpace(destination))
        {
            return sync;
        }

        var baseName = GetRegisterBaseName(destination);
        if (baseName.Length > 1 && baseName[0] is 'r' or 'R' &&
            int.TryParse(baseName.AsSpan(1), out var gpr) && gpr is >= 0 and < 32)
        {
            return sync with { ReloadGprMask = sync.ReloadGprMask & ~(1u << gpr) };
        }

        if (baseName.Length > 1 && baseName[0] is 'f' or 'F' &&
            int.TryParse(baseName.AsSpan(1), out var fpr) && fpr is >= 0 and < 32)
        {
            return sync with { ReloadFprMask = sync.ReloadFprMask & ~(1u << fpr) };
        }

        if (baseName.Equals("cr", StringComparison.OrdinalIgnoreCase))
        {
            return sync with { ReloadsCr = false };
        }

        if (baseName.Equals("ctr", StringComparison.OrdinalIgnoreCase))
        {
            return sync with { ReloadsCtr = false };
        }

        if (baseName.Equals("xer", StringComparison.OrdinalIgnoreCase))
        {
            return sync with { ReloadsXer = false };
        }

        return sync;
    }

    /// <summary>
    /// Rejects a residency whose local names would shadow an emitted temporary. A guard against a
    /// future naming change, not an expected condition, since <see cref="IsCpuRegisterNoLocalNeeded"/>
    /// already keeps machine register names out of the ordinary local set.
    /// </summary>
    private static void RequireResidencyNamesAreFree(
        RegisterResidency residency,
        uint entryPoint,
        IEnumerable<string> emittedLocalNames)
    {
        var taken = new HashSet<string>(emittedLocalNames, StringComparer.Ordinal);
        var collisions = residency.DeclaredLocalNames.Where(taken.Contains).ToArray();
        if (collisions.Length != 0)
        {
            throw new InvalidOperationException(
                $"Register residency local name(s) {string.Join(", ", collisions)} collide with an existing " +
                $"local in 0x{entryPoint:X8}.");
        }
    }
}
