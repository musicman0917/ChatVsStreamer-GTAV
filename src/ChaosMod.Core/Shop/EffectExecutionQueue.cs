using System.Collections.Concurrent;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.Core.Shop
{
    public sealed class QueuedEffectExecution
    {
        public EffectDefinition Definition { get; }
        public EffectExecutionContext ExecutionContext { get; }
        public long? DebitTransactionId { get; }

        public QueuedEffectExecution(EffectDefinition definition, EffectExecutionContext executionContext, long? debitTransactionId)
        {
            Definition = definition;
            ExecutionContext = executionContext;
            DebitTransactionId = debitTransactionId;
        }
    }

    /// <summary>
    /// Thread-safe hand-off point between chat/event callback threads (which
    /// enqueue) and the SHVDN Tick thread (which is the only thread allowed to
    /// dequeue and actually invoke IEffect.Execute).
    /// </summary>
    public sealed class EffectExecutionQueue
    {
        private readonly ConcurrentQueue<QueuedEffectExecution> _queue = new ConcurrentQueue<QueuedEffectExecution>();

        public void Enqueue(QueuedEffectExecution item) => _queue.Enqueue(item);

        public bool TryDequeue(out QueuedEffectExecution item) => _queue.TryDequeue(out item);

        public int Count => _queue.Count;
    }
}
