#ifndef HARMONY_RING_BUFFER_H
#define HARMONY_RING_BUFFER_H

#include "../os_api.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>

// Bounded ring buffer for real-time streaming.
// Pre-allocated slots, bounded memory, overwrites old data when full

typedef struct RingSlot {
    void *data;
    atomic_ulong sequence;  // For ABA protection
} RingSlot;

typedef struct RingBuffer {
    RingSlot *slots;
    size_t capacity;
    
    // Atomic indices
    atomic_size_t write_idx;
    atomic_size_t read_idx;
    
    OS_Mutex *mutex;        // Protects slots and transfers ownership to consumers
    OS_Semaphore *sem;      // Signals consumer
    atomic_bool shutdown;
} RingBuffer;

static inline RingBuffer* RingBuffer_Create(size_t capacity) {
    RingBuffer *rb = (RingBuffer *)calloc(1, sizeof(RingBuffer));
    rb->capacity = capacity;
    rb->slots = (RingSlot *)calloc(capacity, sizeof(RingSlot));
    
    atomic_init(&rb->write_idx, 0);
    atomic_init(&rb->read_idx, 0);
    atomic_init(&rb->shutdown, false);
    
    rb->mutex = OS_MutexCreate();
    rb->sem = OS_SemaphoreCreate(0);
    
    return rb;
}

// Push data. If full, drops oldest item and calls drop_fn if provided.
// On false (full without drop_fn, or shutdown), the caller still owns data.
static inline bool RingBuffer_Push(RingBuffer *rb, void *data, void (*drop_fn)(void*)) {
    OS_MutexLock(rb->mutex);
    if (atomic_load(&rb->shutdown)) {
        OS_MutexUnlock(rb->mutex);
        return false;
    }
    
    size_t write_pos = atomic_load(&rb->write_idx) % rb->capacity;
    size_t read_pos = atomic_load(&rb->read_idx);
    
    // Check if buffer is full (write has lapped read)
    bool is_full = (atomic_load(&rb->write_idx) - read_pos) >= rb->capacity;
    
    if (is_full) {
        if (drop_fn) {
            // Drop the oldest item (at read position)
            size_t drop_pos = read_pos % rb->capacity;
            if (rb->slots[drop_pos].data && drop_fn) {
                drop_fn(rb->slots[drop_pos].data);
            }
            // Advance read position (consumer loses this frame)
            atomic_fetch_add(&rb->read_idx, 1);
        } else {
            OS_MutexUnlock(rb->mutex);
            return false;  // Buffer full, can't add
        }
    }
    
    // Write the data
    rb->slots[write_pos].data = data;
    atomic_fetch_add(&rb->write_idx, 1);
    
    // Replacing an old item does not add another item to wake a consumer for.
    if (!is_full) OS_SemaphorePost(rb->sem);
    OS_MutexUnlock(rb->mutex);
    
    return true;
}

// Pop data. Blocks until available or shutdown.
// Returns NULL on shutdown.
static inline void* RingBuffer_Pop(RingBuffer *rb) {
    for (;;) {
        OS_SemaphoreWait(rb->sem);
        OS_MutexLock(rb->mutex);
        if (atomic_load(&rb->shutdown)) {
            OS_MutexUnlock(rb->mutex);
            return NULL;
        }
        
        size_t read_pos = atomic_load(&rb->read_idx);
        size_t write_pos = atomic_load(&rb->write_idx);
        
        if (read_pos >= write_pos) {
            // TryPop may have consumed the item associated with this wakeup.
            OS_MutexUnlock(rb->mutex);
            continue;
        }
        
        size_t slot_idx = read_pos % rb->capacity;
        void *data = rb->slots[slot_idx].data;
        rb->slots[slot_idx].data = NULL;  // Clear slot
        
        atomic_fetch_add(&rb->read_idx, 1);
        OS_MutexUnlock(rb->mutex);
        return data;
    }
}

// Non-blocking pop. Returns NULL if empty.
static inline void* RingBuffer_TryPop(RingBuffer *rb) {
    OS_MutexLock(rb->mutex);
    size_t read_pos = atomic_load(&rb->read_idx);
    size_t write_pos = atomic_load(&rb->write_idx);
    
    if (read_pos >= write_pos) {
        OS_MutexUnlock(rb->mutex);
        return NULL;  // Empty
    }
    
    size_t slot_idx = read_pos % rb->capacity;
    void *data = rb->slots[slot_idx].data;
    if (data) {
        rb->slots[slot_idx].data = NULL;
        atomic_fetch_add(&rb->read_idx, 1);
    }
    OS_MutexUnlock(rb->mutex);
    return data;
}

static inline size_t RingBuffer_Count(RingBuffer *rb) {
    OS_MutexLock(rb->mutex);
    size_t write_pos = atomic_load(&rb->write_idx);
    size_t read_pos = atomic_load(&rb->read_idx);
    OS_MutexUnlock(rb->mutex);
    return (write_pos > read_pos) ? (write_pos - read_pos) : 0;
}

static inline void RingBuffer_Shutdown(RingBuffer *rb) {
    if (!rb) return;
    OS_MutexLock(rb->mutex);
    atomic_store(&rb->shutdown, true);
    OS_MutexUnlock(rb->mutex);
    // Wake all waiting consumers
    for (int i = 0; i < 8; i++) {
        OS_SemaphorePost(rb->sem);
    }
}

// Drain remaining items, calling free_fn on each
static inline void RingBuffer_Drain(RingBuffer *rb, void (*free_fn)(void*)) {
    if (!rb) return;
    
    void *data;
    while ((data = RingBuffer_TryPop(rb)) != NULL) {
        if (free_fn) free_fn(data);
    }
}

static inline void RingBuffer_Destroy(RingBuffer *rb, void (*free_fn)(void*)) {
    if (!rb) return;
    
    RingBuffer_Shutdown(rb);
    RingBuffer_Drain(rb, free_fn);
    
    free(rb->slots);
    OS_MutexDestroy(rb->mutex);
    OS_SemaphoreDestroy(rb->sem);
    free(rb);
}

#endif // HARMONY_RING_BUFFER_H
