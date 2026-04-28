#include "pipeline/c_packet_queue.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct fh_packet_node {
    fh_packet_t pkt;
    struct fh_packet_node* next;
} fh_packet_node_t;

struct fh_packet_queue {
    size_t max_packets;
    size_t max_bytes;

    pthread_mutex_t mtx;
    pthread_cond_t cv;

    fh_packet_node_t* head;
    fh_packet_node_t* tail;

    size_t dropped_packets;
    size_t dropped_bytes;
    size_t queued_packets;
    size_t queued_bytes;
};

static void packet_zero(fh_packet_t* pkt) {
    // Reset a packet structure after its payload has been released.
    memset(pkt, 0, sizeof(*pkt));
}

void fh_packet_release(fh_packet_t* pkt) {
    // Free the packet payload and clear the struct so it cannot be reused accidentally.
    if (pkt == NULL) {
        return;
    }
    free(pkt->data);
    packet_zero(pkt);
}

static void drop_head_locked(fh_packet_queue_t* q) {
    // Remove the oldest queued packet while holding the queue mutex.
    fh_packet_node_t* old = q->head;
    if (old == NULL) {
        return;
    }

    q->head = old->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }

    q->queued_packets -= 1;
    q->queued_bytes -= old->pkt.caplen;
    q->dropped_packets += 1;
    q->dropped_bytes += old->pkt.caplen;

    fh_packet_release(&old->pkt);
    free(old);
}

fh_packet_queue_t* fh_packet_queue_create(size_t max_packets, size_t max_bytes) {
    // Allocate the queue and initialize its synchronization primitives.
    if (max_packets == 0 || max_bytes == 0) {
        return NULL;
    }

    fh_packet_queue_t* q = (fh_packet_queue_t*)calloc(1, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }

    q->max_packets = max_packets;
    q->max_bytes = max_bytes;

    if (pthread_mutex_init(&q->mtx, NULL) != 0) {
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->cv, NULL) != 0) {
        pthread_mutex_destroy(&q->mtx);
        free(q);
        return NULL;
    }

    return q;
}

void fh_packet_queue_destroy(fh_packet_queue_t* q) {
    // Drain all pending packets, destroy the mutex/condition variable, and free the queue.
    if (q == NULL) {
        return;
    }

    pthread_mutex_lock(&q->mtx);
    while (q->head != NULL) {
        drop_head_locked(q);
    }
    pthread_mutex_unlock(&q->mtx);

    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->mtx);
    free(q);
}

int fh_packet_queue_push_copy(fh_packet_queue_t* q, const fh_packet_t* pkt) {
    // Copy the incoming packet into queue-owned memory so the capture buffer can be reused.
    if (q == NULL || pkt == NULL || pkt->data == NULL) {
        return -1;
    }

    // If the packet is too large to ever fit, count it as dropped immediately.
    if (pkt->caplen > q->max_bytes) {
        pthread_mutex_lock(&q->mtx);
        q->dropped_packets += 1;
        q->dropped_bytes += pkt->caplen;
        pthread_mutex_unlock(&q->mtx);
        return 1;
    }

    fh_packet_node_t* node = (fh_packet_node_t*)calloc(1, sizeof(*node));
    if (node == NULL) {
        return -1;
    }

    node->pkt = *pkt;
    node->pkt.data = (uint8_t*)malloc(pkt->caplen);
    if (node->pkt.data == NULL) {
        free(node);
        return -1;
    }
    memcpy(node->pkt.data, pkt->data, pkt->caplen);

    // Enforce queue limits by dropping oldest packets until the new one fits.
    pthread_mutex_lock(&q->mtx);
    while (q->head != NULL && (q->queued_packets >= q->max_packets || q->queued_bytes + pkt->caplen > q->max_bytes)) {
        drop_head_locked(q);
    }

    // Link the new node at the tail of the queue.
    if (q->tail == NULL) {
        q->head = node;
        q->tail = node;
    } else {
        q->tail->next = node;
        q->tail = node;
    }

    q->queued_packets += 1;
    q->queued_bytes += pkt->caplen;

    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

int fh_packet_queue_pop(fh_packet_queue_t* q, fh_packet_t* out, const volatile sig_atomic_t* stop_flag) {
    // Wait until a packet arrives or shutdown is requested.
    if (q == NULL || out == NULL || stop_flag == NULL) {
        return -1;
    }

    pthread_mutex_lock(&q->mtx);
    while (q->head == NULL && *stop_flag == 0) {
        // Use a short timed wait so the loop remains responsive to stop_flag.
        struct timespec wait_until;
        (void)timespec_get(&wait_until, TIME_UTC);
        wait_until.tv_nsec += 100L * 1000L * 1000L;
        if (wait_until.tv_nsec >= 1000000000L) {
            wait_until.tv_sec += 1;
            wait_until.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&q->cv, &q->mtx, &wait_until);
    }

    if (q->head == NULL) {
        pthread_mutex_unlock(&q->mtx);
        return 0;
    }

    // Detach the head node and transfer ownership of its packet to the caller.
    fh_packet_node_t* node = q->head;
    q->head = node->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }

    q->queued_packets -= 1;
    q->queued_bytes -= node->pkt.caplen;

    *out = node->pkt;
    free(node);

    pthread_mutex_unlock(&q->mtx);
    return 1;
}

void fh_packet_queue_notify_all(fh_packet_queue_t* q) {
    // Wake all waiting consumers during shutdown.
    if (q == NULL) {
        return;
    }

    pthread_mutex_lock(&q->mtx);
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mtx);
}

fh_queue_stats_t fh_packet_queue_stats(fh_packet_queue_t* q) {
    // Snapshot queue counters under the mutex.
    fh_queue_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    if (q == NULL) {
        return stats;
    }

    pthread_mutex_lock(&q->mtx);
    stats.dropped_packets = q->dropped_packets;
    stats.dropped_bytes = q->dropped_bytes;
    stats.queued_packets = q->queued_packets;
    stats.queued_bytes = q->queued_bytes;
    pthread_mutex_unlock(&q->mtx);

    return stats;
}
