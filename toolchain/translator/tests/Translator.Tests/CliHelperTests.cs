using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Xunit;

namespace Translator.Tests;

public sealed class CliHelperTests
{
    [Fact]
    public void IndexedParallelPreservesIndexedResultsAcrossExecutionModes()
    {
        var sequential = new int[8];
        var parallel = new int[8];

        IndexedParallel.For(
            sequential.Length,
            new ParallelOptions { MaxDegreeOfParallelism = 1 },
            index => sequential[index] = index * 3);
        IndexedParallel.For(
            parallel.Length,
            new ParallelOptions { MaxDegreeOfParallelism = 4 },
            index => parallel[index] = index * 3);

        Assert.Equal(sequential, parallel);
        Assert.Equal(Enumerable.Range(0, sequential.Length).Select(index => index * 3), sequential);
    }

    [Fact]
    public void IndexedParallelUnwrapsA_singleWorkerFailure()
    {
        var exception = Assert.Throws<InvalidOperationException>(() => IndexedParallel.For(
            8,
            new ParallelOptions { MaxDegreeOfParallelism = 4 },
            index =>
            {
                if (index == 0)
                {
                    throw new InvalidOperationException("indexed failure");
                }
            }));

        Assert.Equal("indexed failure", exception.Message);
    }

    [Fact]
    public void QueueBatchPreservesFifoAndSkipsUntilTheBatchIsFull()
    {
        var queue = new Queue<int>(new[] { 1, 2, 3, 4, 5 });

        var first = QueueBatch.Dequeue(queue, 2, value => value % 2 == 0);

        Assert.Equal(new[] { 1, 3 }, first);
        Assert.Equal(new[] { 4, 5 }, queue);
    }
}
