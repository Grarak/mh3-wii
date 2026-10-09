using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Translator.Core.Disassembly;
using Translator.Core.Translation;

/// <summary>One function a mod translation wave has to emit.</summary>
sealed record ModTranslationWork(
    uint Address,
    TranslationOptions Options,
    string OutputPath);

/// <summary>
/// The outcome of one work item: either a translation or the message explaining
/// why that address could not be translated. Failures are values, not
/// exceptions, so one bad address cannot abandon the rest of its wave.
/// </summary>
sealed record ModTranslationAttempt(
    ModTranslationWork Work,
    FunctionTranslationResult? Result,
    string? Error);

/// <summary>
/// Translates one wave of mod work items in parallel and returns the attempts in
/// a deterministic (Address, OutputPath) order, so the emitted bundle never
/// depends on thread count or completion order.
/// </summary>
static class ModTranslationWaveRunner
{
    internal static ModTranslationAttempt[] Translate(
        Func<FunctionTranslator> translatorFactory,
        IReadOnlyList<ModTranslationWork> wave,
        ParallelOptions parallelOptions)
    {
        var attempts = new ModTranslationAttempt[wave.Count];
        void TranslateAt(int index)
        {
            var work = wave[index];
            try
            {
                attempts[index] = new ModTranslationAttempt(
                    work,
                    translatorFactory().Translate(work.Address, work.Options),
                    null);
            }
            catch (Exception ex) when (ex is InvalidOperationException or ArgumentOutOfRangeException or PpcDisassemblyLimitExceededException)
            {
                attempts[index] = new ModTranslationAttempt(
                    work,
                    null,
                    $"0x{work.Address:X8} {work.Options.PreferredName}: {ex.Message}");
            }
        }

        IndexedParallel.For(wave.Count, parallelOptions, TranslateAt);

        return attempts
            .OrderBy(attempt => attempt.Work.Address)
            .ThenBy(attempt => attempt.Work.OutputPath, StringComparer.Ordinal)
            .ToArray();
    }
}
