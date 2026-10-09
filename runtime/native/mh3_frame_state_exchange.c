#include "mh3_frame_state_exchange.h"

/* Atomic exchange linearizes both ownership transfers. Ready is cleared only
 * by the consumer, and set only by the producer. Payload writes precede the
 * release; taking the old middle acquires the preceding owner's last writes.
 * Thus back, middle and front are always a permutation of the three buffers. */
enum {
    INDEX_MASK = 3,
    READY = 4,
    STOP = 1,
    PRODUCER_IDLE = 2,
    CONSUMER_IDLE = 4,
    RESETTING = 8
};

_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "32-bit lock-free atomics required");
static uint32_t lifecycle(const mh3_frame_state_exchange *x) {
    return __atomic_load_n(&x->lifecycle, __ATOMIC_ACQUIRE);
}

static int overlap(uintptr_t a, size_t an, uintptr_t b, size_t bn) {
    return a < b + bn && b < a + an;
}

static mh3_frame_state_status producer(mh3_frame_state_producer *p) {
    if (!p || !p->owner || p->owner->producer != p)
        return MH3_FRAME_STATE_INVALID;

    if (lifecycle(p->owner))
        return MH3_FRAME_STATE_STOPPED;

    return p->index < 3 ? MH3_FRAME_STATE_OK : MH3_FRAME_STATE_INVALID;
}

static mh3_frame_state_status consumer(mh3_frame_state_consumer *c) {
    if (!c || !c->owner || c->owner->consumer != c)
        return MH3_FRAME_STATE_INVALID;

    if (lifecycle(c->owner))
        return MH3_FRAME_STATE_STOPPED;

    return c->index < 3 ? MH3_FRAME_STATE_OK : MH3_FRAME_STATE_INVALID;
}

mh3_frame_state_status mh3_frame_state_init(mh3_frame_state_exchange *x, mh3_frame_state_producer *p, mh3_frame_state_consumer *c, void *a, void *b, void *d, size_t bytes) {
    uintptr_t starts[6] = {(uintptr_t)a, (uintptr_t)b, (uintptr_t)d, (uintptr_t)x, (uintptr_t)p, (uintptr_t)c};
    size_t sizes[6] = {bytes, bytes, bytes, sizeof(*x), sizeof(*p), sizeof(*c)};
    unsigned i, j;

    if (!x || !p || !c || !a || !b || !d || !bytes)
        return MH3_FRAME_STATE_INVALID;

    for (i = 0; i < 6; i++) {
        if (sizes[i] > UINTPTR_MAX - starts[i])
            return MH3_FRAME_STATE_INVALID;

        for (j = 0; j < i; j++)
            if (overlap(starts[i], sizes[i], starts[j], sizes[j]))
                return MH3_FRAME_STATE_INVALID;
    }

    x->payload[0] = a;
    x->payload[1] = b;
    x->payload[2] = d;
    x->bytes = bytes;
    x->generation[0] = x->generation[1] = x->generation[2] = 0;
    x->producer = p;
    x->consumer = c;
    p->owner = x;
    p->index = 0;
    c->owner = x;
    c->index = 2;
    c->valid = 0;
    c->generation = 0;
    __atomic_store_n(&x->middle, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&x->lifecycle, 0, __ATOMIC_RELEASE);
    return MH3_FRAME_STATE_OK;
}

mh3_frame_state_status mh3_frame_state_producer_buffer(mh3_frame_state_producer *p, void **data, size_t *bytes) {
    mh3_frame_state_status status = producer(p);

    if (status != MH3_FRAME_STATE_OK)
        return status;

    if (!data || !bytes)
        return MH3_FRAME_STATE_INVALID;

    *data = p->owner->payload[p->index];
    *bytes = p->owner->bytes;
    return MH3_FRAME_STATE_OK;
}

mh3_frame_state_status mh3_frame_state_publish(mh3_frame_state_producer *p, uint64_t generation, uint32_t *replaced) {
    mh3_frame_state_status status = producer(p);
    uint32_t previous;

    if (status != MH3_FRAME_STATE_OK)
        return status;

    p->owner->generation[p->index] = generation;
    previous = __atomic_exchange_n(&p->owner->middle, p->index | READY, __ATOMIC_ACQ_REL);
    p->index = previous & INDEX_MASK;

    if (replaced)
        *replaced = (previous & READY) != 0;

    return MH3_FRAME_STATE_OK;
}

mh3_frame_state_status mh3_frame_state_acquire(mh3_frame_state_consumer *c) {
    mh3_frame_state_status status = consumer(c);
    uint32_t previous;

    if (status != MH3_FRAME_STATE_OK)
        return status;

    if (!(__atomic_load_n(&c->owner->middle, __ATOMIC_ACQUIRE) & READY))
        return MH3_FRAME_STATE_EMPTY;
    /* With one consumer READY cannot clear between this load and exchange.
     * A racing producer can only replace middle with an even newer input. */
    previous = __atomic_exchange_n(&c->owner->middle, c->index, __ATOMIC_ACQ_REL);
    c->index = previous & INDEX_MASK;
    c->generation = c->owner->generation[c->index];
    c->valid = 1;
    return MH3_FRAME_STATE_OK;
}

mh3_frame_state_status mh3_frame_state_consumer_buffer(mh3_frame_state_consumer *c, void **data, size_t *bytes, uint64_t *generation) {
    mh3_frame_state_status status = consumer(c);

    if (status != MH3_FRAME_STATE_OK)
        return status;

    if (!data || !bytes || !generation)
        return MH3_FRAME_STATE_INVALID;

    if (!c->valid)
        return MH3_FRAME_STATE_EMPTY;

    *data = c->owner->payload[c->index];
    *bytes = c->owner->bytes;
    *generation = c->generation;
    return MH3_FRAME_STATE_OK;
}

mh3_frame_state_status mh3_frame_state_stop(mh3_frame_state_exchange *x) {
    if (!x)
        return MH3_FRAME_STATE_INVALID;

    __atomic_fetch_or(&x->lifecycle, STOP, __ATOMIC_ACQ_REL);
    return MH3_FRAME_STATE_OK;
}

mh3_frame_state_status mh3_frame_state_producer_idle(mh3_frame_state_producer *p) {
    if (!p || !p->owner || p->owner->producer != p)
        return MH3_FRAME_STATE_INVALID;

    if (!(lifecycle(p->owner) & STOP))
        return MH3_FRAME_STATE_BUSY;

    __atomic_fetch_or(&p->owner->lifecycle, PRODUCER_IDLE, __ATOMIC_RELEASE);
    return MH3_FRAME_STATE_OK;
}

mh3_frame_state_status mh3_frame_state_consumer_idle(mh3_frame_state_consumer *c) {
    if (!c || !c->owner || c->owner->consumer != c)
        return MH3_FRAME_STATE_INVALID;

    if (!(lifecycle(c->owner) & STOP))
        return MH3_FRAME_STATE_BUSY;

    __atomic_fetch_or(&c->owner->lifecycle, CONSUMER_IDLE, __ATOMIC_RELEASE);
    return MH3_FRAME_STATE_OK;
}

mh3_frame_state_status mh3_frame_state_reset(mh3_frame_state_exchange *x) {
    uint32_t before = STOP | PRODUCER_IDLE | CONSUMER_IDLE;

    if (!x)
        return MH3_FRAME_STATE_INVALID;

    if (!__atomic_compare_exchange_n(&x->lifecycle, &before, RESETTING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return MH3_FRAME_STATE_BUSY;
    /* Both roles acknowledged they no longer touch indices or payloads. The
     * controller resumes them only after this release-store and its own signal. */
    x->producer->index = 0;
    x->consumer->index = 2;
    x->consumer->valid = 0;
    x->consumer->generation = 0;
    __atomic_store_n(&x->middle, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&x->lifecycle, 0, __ATOMIC_RELEASE);
    return MH3_FRAME_STATE_OK;
}
