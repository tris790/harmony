#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <signal.h>
#include "../src/core/ring_buffer.h"
#include "../src/platform/linux_threading.c"

enum { ITEM_COUNT = 200000, PAYLOAD_SIZE = 128 };

typedef struct TestItem {
    size_t id;
    size_t payload[PAYLOAD_SIZE];
} TestItem;

typedef struct TestQueue {
    RingBuffer *queue;
    atomic_size_t consumed;
    atomic_size_t dropped;
} TestQueue;

static TestQueue test;

static TestItem *NewItem(size_t id) {
    TestItem *item = malloc(sizeof(*item));
    assert(item);
    item->id = id;
    for (size_t i = 0; i < PAYLOAD_SIZE; i++) item->payload[i] = id;
    return item;
}

static void DropItem(void *data) {
    atomic_fetch_add(&test.dropped, 1);
    free(data);
}

static void *Consume(void *data) {
    bool nonblocking = (uintptr_t)data != 0;
    size_t last_id = 0;
    for (;;) {
        TestItem *item = nonblocking ? RingBuffer_TryPop(test.queue)
                                    : RingBuffer_Pop(test.queue);
        if (!item && nonblocking) {
            sched_yield();
            continue;
        }
        assert(item);
        size_t id = item->id;
        assert(id > last_id);
        for (size_t i = 0; i < PAYLOAD_SIZE; i++) assert(item->payload[i] == id);
        free(item);
        atomic_fetch_add(&test.consumed, 1);
        last_id = id;
        if (id == ITEM_COUNT) return NULL;
    }
}

static void TestConcurrentOverflow(bool nonblocking) {
    test.queue = RingBuffer_Create(4);
    atomic_store(&test.consumed, 0);
    atomic_store(&test.dropped, 0);
    pthread_t consumer;
    assert(pthread_create(&consumer, NULL, Consume, (void *)(uintptr_t)nonblocking) == 0);
    for (size_t id = 1; id <= ITEM_COUNT; id++) {
        assert(RingBuffer_Push(test.queue, NewItem(id), DropItem));
    }
    assert(pthread_join(consumer, NULL) == 0);
    assert(atomic_load(&test.consumed) + atomic_load(&test.dropped) == ITEM_COUNT);
    assert(RingBuffer_Count(test.queue) == 0);
    RingBuffer_Destroy(test.queue, DropItem);

    // Shutdown leaves queued items for the owner to drain after joining workers.
    test.queue = RingBuffer_Create(2);
    atomic_store(&test.dropped, 0);
    assert(RingBuffer_Push(test.queue, NewItem(1), DropItem));
    assert(RingBuffer_Push(test.queue, NewItem(2), DropItem));
    RingBuffer_Shutdown(test.queue);
    assert(RingBuffer_Pop(test.queue) == NULL);
    RingBuffer_Destroy(test.queue, DropItem);
    assert(atomic_load(&test.dropped) == 2);
}

static void *WaitForShutdown(void *data) {
    assert(RingBuffer_Pop(data) == NULL);
    return NULL;
}

static void TestOverflowAndShutdown(void) {
    RingBuffer *queue = RingBuffer_Create(2);
    int values[] = {1, 2, 3};
    assert(RingBuffer_Push(queue, &values[0], NULL));
    assert(RingBuffer_Push(queue, &values[1], NULL));
    assert(!RingBuffer_Push(queue, &values[2], NULL));
    assert(RingBuffer_Pop(queue) == &values[0]);
    assert(RingBuffer_TryPop(queue) == &values[1]);
    assert(RingBuffer_TryPop(queue) == NULL);

    pthread_t consumer;
    assert(pthread_create(&consumer, NULL, WaitForShutdown, queue) == 0);
    RingBuffer_Shutdown(queue);
    assert(pthread_join(consumer, NULL) == 0);
    assert(!RingBuffer_Push(queue, &values[2], NULL));
    RingBuffer_Destroy(queue, NULL);
}

static void TestDropOldest(void) {
    test.queue = RingBuffer_Create(2);
    atomic_store(&test.dropped, 0);
    for (size_t id = 1; id <= 1000; id++) {
        assert(RingBuffer_Push(test.queue, NewItem(id), DropItem));
        assert(RingBuffer_Count(test.queue) <= 2);
    }
    assert(atomic_load(&test.dropped) == 998);
    TestItem *first = RingBuffer_Pop(test.queue);
    TestItem *second = RingBuffer_Pop(test.queue);
    assert(first->id == 999 && second->id == 1000);
    free(first);
    free(second);
    assert(RingBuffer_Count(test.queue) == 0);
    RingBuffer_Destroy(test.queue, DropItem);
}

int main(void) {
    alarm(30); // A queue regression must fail instead of hanging the test suite.
    TestConcurrentOverflow(false);
    TestConcurrentOverflow(true);
    TestDropOldest();
    TestOverflowAndShutdown();
    puts("Ring buffer overflow, ownership, and shutdown tests passed.");
    return 0;
}
