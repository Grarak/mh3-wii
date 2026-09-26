using System;
using System.Collections.Generic;

/// <summary>
/// Small FIFO primitive shared by the recursive and mod wave schedulers.
/// Traversal-specific policy (depth, visited sets, retries, and failure
/// handling) intentionally remains with each caller.
/// </summary>
internal static class QueueBatch
{
    internal static List<T> Dequeue<T>(
        Queue<T> queue,
        int maximumCount,
        Func<T, bool>? skip = null)
    {
        if (maximumCount <= 0)
        {
            return new List<T>();
        }

        var batch = new List<T>(Math.Min(queue.Count, maximumCount));
        while (queue.Count > 0 && batch.Count < maximumCount)
        {
            var item = queue.Dequeue();
            if (skip is not null && skip(item))
            {
                continue;
            }

            batch.Add(item);
        }

        return batch;
    }
}
