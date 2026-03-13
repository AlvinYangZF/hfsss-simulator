#ifndef __HFSSS_MSGQUEUE_H
#define __HFSSS_MSGQUEUE_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Message Queue Context */
struct msg_queue {
    u32 msg_size;
    u32 queue_len;
    u32 count;
    u32 head;
    u32 tail;
    u8 *buffer;
    void *lock;
    void *not_empty;
    void *not_full;
    u64 send_count;
    u64 recv_count;
};

/* Function Prototypes */
int msg_queue_init(struct msg_queue *mq, u32 msg_size, u32 queue_len);
void msg_queue_cleanup(struct msg_queue *mq);
int msg_queue_send(struct msg_queue *mq, const void *msg, u64 timeout_ns);
int msg_queue_recv(struct msg_queue *mq, void *msg, u64 timeout_ns);
int msg_queue_trysend(struct msg_queue *mq, const void *msg);
int msg_queue_tryrecv(struct msg_queue *mq, void *msg);
u32 msg_queue_count(struct msg_queue *mq);
void msg_queue_stats(struct msg_queue *mq, u64 *send_count, u64 *recv_count);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_MSGQUEUE_H */
