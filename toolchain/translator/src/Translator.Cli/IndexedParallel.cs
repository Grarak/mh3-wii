using System;
using System.Threading.Tasks;

/// <summary>
/// Runs an indexed operation with the same sequential behavior used by the
/// translator's parallel stages. Keeping the sequential path explicit makes
/// single-threaded runs deterministic and avoids AggregateException wrapping.
/// </summary>
internal static class IndexedParallel
{
    internal static void For(int count, ParallelOptions parallelOptions, Action<int> body)
    {
        if (count <= 0)
        {
            return;
        }

        if (parallelOptions.MaxDegreeOfParallelism <= 1 || count == 1)
        {
            for (var index = 0; index < count; index++)
            {
                body(index);
            }

            return;
        }

        try
        {
            Parallel.For(0, count, parallelOptions, body);
        }
        catch (AggregateException ex)
        {
            // Preserve the recursive translator's historical behavior: a
            // single worker failure is reported as that failure, while a
            // genuinely multi-failure pass remains aggregate-shaped.
            throw ex.InnerExceptions.Count == 1 ? ex.InnerExceptions[0] : ex;
        }
    }
}
