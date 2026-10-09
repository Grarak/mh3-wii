#ifndef MH3_FRAME_STATE_EXCHANGE_H
#define MH3_FRAME_STATE_EXCHANGE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Three caller-owned CPU buffers: producer private back, shared middle and
 * renderer private front. Only middle_index/ready use an atomic ownership
 * exchange. The private owners may freely mutate their own payloads.
 * Published inputs must own/pin everything they refer to. GPU-visible bytes
 * require separate GPU completion leases; they must not borrow these buffers.
 * One producer, one consumer, one lifecycle controller. Do not copy handles. */
typedef struct mh3_frame_state_exchange mh3_frame_state_exchange;
typedef struct {
    mh3_frame_state_exchange *owner;
    uint32_t index;
} mh3_frame_state_producer;
typedef struct {
    mh3_frame_state_exchange *owner;
    uint32_t index, valid;
    uint64_t generation;
} mh3_frame_state_consumer;
struct mh3_frame_state_exchange {
    uint32_t middle, lifecycle;
    void *payload[3];
    size_t bytes;
    uint64_t generation[3];
    mh3_frame_state_producer *producer;
    mh3_frame_state_consumer *consumer;
};
typedef enum {
    MH3_FRAME_STATE_OK=0,
    MH3_FRAME_STATE_EMPTY,
    MH3_FRAME_STATE_BUSY,
    MH3_FRAME_STATE_STOPPED,
    MH3_FRAME_STATE_INVALID
} mh3_frame_state_status;

/* Initialize only while quiescent. Buffers and handle objects must be distinct,
 * nonoverlapping, and allocated for the entire exchange lifetime. Generation
 * values belong to the caller. Failed operations leave output arguments intact.
 * Recycled producer buffers may contain old renderer scratch: rebuild the full
 * logical input state before publishing; do not rely on inherited slot bytes. */
mh3_frame_state_status mh3_frame_state_init(mh3_frame_state_exchange *,
    mh3_frame_state_producer *,mh3_frame_state_consumer *,void *,void *,void *,size_t);
mh3_frame_state_status mh3_frame_state_producer_buffer(mh3_frame_state_producer *,void **,size_t *);
mh3_frame_state_status mh3_frame_state_publish(mh3_frame_state_producer *,uint64_t,uint32_t *replaced);
/* Acquire swaps the renderer's old private front with the newest pending
 * middle. EMPTY keeps the existing front. It creates no shared/frozen lease. */
mh3_frame_state_status mh3_frame_state_acquire(mh3_frame_state_consumer *);
mh3_frame_state_status mh3_frame_state_consumer_buffer(mh3_frame_state_consumer *,void **,size_t *,uint64_t *);
/* Stop can race a publish/acquire already in progress: that operation may
 * finish. Both owners may finish using their existing private pointers, then
 * acknowledge idle. After idle, no payload/role access is permitted until reset
 * completes and the lifecycle controller explicitly resumes the role threads.
 * Reset is nonblocking and requires BOTH idle acknowledgments. The lifecycle
 * controller must serialize stop/reset/init/retirement. Payload bytes persist. */
mh3_frame_state_status mh3_frame_state_stop(mh3_frame_state_exchange *);
mh3_frame_state_status mh3_frame_state_producer_idle(mh3_frame_state_producer *);
mh3_frame_state_status mh3_frame_state_consumer_idle(mh3_frame_state_consumer *);
mh3_frame_state_status mh3_frame_state_reset(mh3_frame_state_exchange *);

#ifdef __cplusplus
}
#endif
#endif
