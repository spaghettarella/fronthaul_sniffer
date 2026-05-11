#include "c_packet_queue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum fh_packet_slot_state {
    FH_PACKET_SLOT_FREE = 0,
    FH_PACKET_SLOT_READY,
    FH_PACKET_SLOT_IN_FLIGHT,
} fh_packet_slot_state_t;

typedef struct fh_packet_slot {
    fh_packet_t pkt;
    struct fh_packet_queue* owner;
    uint8_t* storage;
    size_t storage_capacity;
    size_t stored_len;
    int next_ready;
    int next_free;
    fh_packet_slot_state_t state;
} fh_packet_slot_t;

struct fh_packet_queue {
    size_t max_packets;
    size_t max_packet_bytes;
    size_t slot_capacity;

    pthread_mutex_t mtx;
    pthread_cond_t cv;

    fh_packet_slot_t* slots;
    uint8_t* storage;
    int free_head;
    int ready_head;
    int ready_tail;

    size_t dropped_packets;
    size_t dropped_bytes;
    size_t total_packets;
    size_t processed_packets;
    size_t queued_packets;
    size_t queued_bytes;
    size_t ready_packets;
};

static void packet_zero(fh_packet_t* pkt) {
    if (pkt == NULL) {
        return;
    }
    memset(pkt, 0, sizeof(*pkt));
}

static void fh_packet_queue_return_slot_locked(fh_packet_queue_t* q, fh_packet_slot_t* slot) {
    if (q == NULL || slot == NULL) {
        return;
    }

    slot->state = FH_PACKET_SLOT_FREE;
    slot->pkt.data = NULL;
    slot->pkt.caplen = 0;
    slot->pkt.len = 0;
    slot->pkt.release_cb = NULL;
    slot->pkt.release_ctx = NULL;
    slot->stored_len = 0;

    slot->next_free = q->free_head;
    q->free_head = (int)(slot - q->slots);
}

static void fh_packet_queue_release_packet(void* release_ctx, struct fh_packet* pkt) {
    if (release_ctx == NULL || pkt == NULL) {
        return;
    }

    fh_packet_slot_t* slot = (fh_packet_slot_t*)release_ctx;
    fh_packet_queue_t* q = slot->owner;
    if (q == NULL) {
        return;
    }

    pthread_mutex_lock(&q->mtx);
    if (slot->state == FH_PACKET_SLOT_IN_FLIGHT || slot->state == FH_PACKET_SLOT_READY) {
        if (q->queued_packets > 0) {
            q->queued_packets -= 1;
        }
        if (q->queued_bytes >= slot->stored_len) {
            q->queued_bytes -= slot->stored_len;
        } else {
            q->queued_bytes = 0;
        }
    }
    fh_packet_queue_return_slot_locked(q, slot);
    pthread_mutex_unlock(&q->mtx);

    packet_zero(pkt);
}

void fh_packet_release(fh_packet_t* pkt) {
    // Free queue-backed packets through their release callback; otherwise free payload memory.
    if (pkt == NULL) {
        return;
    }

    if (pkt->release_cb != NULL) {
        void (*release_cb)(void*, struct fh_packet*) = pkt->release_cb;
        void* release_ctx = pkt->release_ctx;
        release_cb(release_ctx, pkt);
        return;
    }

    free(pkt->data);
    packet_zero(pkt);
}

static void drop_ready_head_locked(fh_packet_queue_t* q) {
    if (q == NULL || q->ready_head < 0) {
        return;
    }

    fh_packet_slot_t* slot = &q->slots[q->ready_head];
    if (slot->state != FH_PACKET_SLOT_READY) {
        return;
    }

    const size_t dropped_len = slot->stored_len;
    q->dropped_packets += 1;
    q->dropped_bytes += dropped_len;
    if (q->queued_packets > 0) {
        q->queued_packets -= 1;
    }
    if (q->ready_packets > 0) {
        q->ready_packets -= 1;
    }
    if (q->queued_bytes >= dropped_len) {
        q->queued_bytes -= dropped_len;
    } else {
        q->queued_bytes = 0;
    }

    int next_ready = slot->next_ready;
    slot->state = FH_PACKET_SLOT_FREE;
    slot->pkt.data = NULL;
    slot->stored_len = 0;
    slot->next_ready = -1;
    slot->pkt.release_cb = NULL;
    slot->pkt.release_ctx = NULL;

    slot->next_free = q->free_head;
    q->free_head = q->ready_head;
    q->ready_head = next_ready;
    if (q->ready_head < 0) {
        q->ready_tail = -1;
    }
}

fh_packet_queue_t* fh_packet_queue_create(size_t max_packets, size_t max_bytes) {
    if (max_packets == 0 || max_bytes == 0) {
        return NULL;
    }

    fh_packet_queue_t* q = (fh_packet_queue_t*)calloc(1, sizeof(*q));
    if (q == NULL) {
        return NULL;
    }

    q->max_packets = max_packets;
    q->max_packet_bytes = max_bytes;
    q->slot_capacity = max_bytes;

    q->slots = (fh_packet_slot_t*)calloc(max_packets, sizeof(*q->slots));
    if (q->slots == NULL) {
        free(q);
        return NULL;
    }

    q->storage = (uint8_t*)calloc(max_packets, q->slot_capacity);
    if (q->storage == NULL) {
        free(q->slots);
        free(q);
        return NULL;
    }

    for (size_t i = 0; i < max_packets; ++i) {
        fh_packet_slot_t* slot = &q->slots[i];
        slot->owner = q;
        slot->storage = q->storage + (i * q->slot_capacity);
        slot->storage_capacity = q->slot_capacity;
        slot->state = FH_PACKET_SLOT_FREE;
        slot->next_free = (i + 1 < max_packets) ? (int)(i + 1) : -1;
        slot->next_ready = -1;
    }

    q->free_head = 0;
    q->ready_head = -1;
    q->ready_tail = -1;

    if (pthread_mutex_init(&q->mtx, NULL) != 0) {
        free(q->storage);
        free(q->slots);
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->cv, NULL) != 0) {
        pthread_mutex_destroy(&q->mtx);
        free(q->storage);
        free(q->slots);
        free(q);
        return NULL;
    }

    return q;
}

void fh_packet_queue_destroy(fh_packet_queue_t* q) {
    if (q == NULL) {
        return;
    }

    pthread_mutex_lock(&q->mtx);
    pthread_mutex_unlock(&q->mtx);

    pthread_cond_destroy(&q->cv);
    pthread_mutex_destroy(&q->mtx);
    free(q->storage);
    free(q->slots);
    free(q);
}

int fh_packet_queue_push_copy(fh_packet_queue_t* q, const fh_packet_t* pkt) {
    if (q == NULL || pkt == NULL || pkt->data == NULL) {
        return -1;
    }

    pthread_mutex_lock(&q->mtx);
    q->total_packets += 1;

    if (pkt->caplen > q->max_packet_bytes) {
        q->dropped_packets += 1;
        q->dropped_bytes += pkt->caplen;
        pthread_mutex_unlock(&q->mtx);
        return 1;
    }

    while (q->free_head < 0) {
        if (q->ready_head < 0) {
            q->dropped_packets += 1;
            q->dropped_bytes += pkt->caplen;
            pthread_mutex_unlock(&q->mtx);
            return 1;
        }
        drop_ready_head_locked(q);
    }

    const int slot_index = q->free_head;
    fh_packet_slot_t* slot = &q->slots[slot_index];
    q->free_head = slot->next_free;
    slot->next_free = -1;

    memcpy(slot->storage, pkt->data, pkt->caplen);
    slot->stored_len = pkt->caplen;
    slot->pkt = *pkt;
    slot->pkt.data = slot->storage;
    slot->pkt.release_cb = fh_packet_queue_release_packet;
    slot->pkt.release_ctx = slot;
    slot->state = FH_PACKET_SLOT_READY;
    slot->next_ready = -1;

    if (q->ready_tail < 0) {
        q->ready_head = slot_index;
        q->ready_tail = slot_index;
    } else {
        q->slots[q->ready_tail].next_ready = slot_index;
        q->ready_tail = slot_index;
    }

    q->ready_packets += 1;
    q->queued_packets += 1;
    q->queued_bytes += pkt->caplen;

    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

int fh_packet_queue_pop(fh_packet_queue_t* q, fh_packet_t* out, const volatile sig_atomic_t* stop_flag) {
    if (q == NULL || out == NULL || stop_flag == NULL) {
        return -1;
    }

    pthread_mutex_lock(&q->mtx);
    while (q->ready_head < 0 && *stop_flag == 0) {
        struct timespec wait_until;
        (void)timespec_get(&wait_until, TIME_UTC);
        wait_until.tv_nsec += 100L * 1000L * 1000L;
        if (wait_until.tv_nsec >= 1000000000L) {
            wait_until.tv_sec += 1;
            wait_until.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&q->cv, &q->mtx, &wait_until);
    }

    if (q->ready_head < 0) {
        pthread_mutex_unlock(&q->mtx);
        return 0;
    }

    const int slot_index = q->ready_head;
    fh_packet_slot_t* slot = &q->slots[slot_index];
    q->ready_head = slot->next_ready;
    if (q->ready_head < 0) {
        q->ready_tail = -1;
    }
    slot->next_ready = -1;
    slot->state = FH_PACKET_SLOT_IN_FLIGHT;
    if (q->ready_packets > 0) {
        q->ready_packets -= 1;
    }

    *out = slot->pkt;
    q->processed_packets += 1;

    pthread_mutex_unlock(&q->mtx);
    return 1;
}

void fh_packet_queue_notify_all(fh_packet_queue_t* q) {
    if (q == NULL) {
        return;
    }

    pthread_mutex_lock(&q->mtx);
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mtx);
}

fh_queue_stats_t fh_packet_queue_stats(fh_packet_queue_t* q) {
    fh_queue_stats_t stats;
    memset(&stats, 0, sizeof(stats));

    if (q == NULL) {
        return stats;
    }

    pthread_mutex_lock(&q->mtx);
    stats.total_packets = q->total_packets;
    stats.processed_packets = q->processed_packets;
    stats.dropped_packets = q->dropped_packets;
    stats.dropped_bytes = q->dropped_bytes;
    stats.queued_packets = q->queued_packets;
    stats.queued_bytes = q->queued_bytes;
    pthread_mutex_unlock(&q->mtx);

    return stats;
}
