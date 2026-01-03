#ifndef HARMONY_QUEUE_H
#define HARMONY_QUEUE_H

#include "../os_api.h"
#include <stdlib.h>
#include <string.h>

typedef struct Node {
    void *data;
    struct Node *next;
} Node;

typedef struct Queue {
    Node *head;
    Node *tail;
    OS_Mutex *mutex;
    OS_Semaphore *sem;
    int count;
} Queue;

static inline Queue* Queue_Create(void) {
    Queue *q = (Queue *)malloc(sizeof(Queue));
    q->head = q->tail = NULL;
    q->mutex = OS_MutexCreate();
    q->sem = OS_SemaphoreCreate(0);
    q->count = 0;
    return q;
}

static inline void Queue_Push(Queue *q, void *data) {
    Node *node = (Node *)malloc(sizeof(Node));
    node->data = data;
    node->next = NULL;

    OS_MutexLock(q->mutex);
    if (q->tail) {
        q->tail->next = node;
        q->tail = node;
    } else {
        q->head = q->tail = node;
    }
    q->count++;
    OS_MutexUnlock(q->mutex);
    OS_SemaphorePost(q->sem);
}

static inline void* Queue_Pop(Queue *q) {
    OS_SemaphoreWait(q->sem);
    OS_MutexLock(q->mutex);
    Node *node = q->head;
    void *data = node->data;
    q->head = node->next;
    if (q->head == NULL) {
        q->tail = NULL;
    }
    q->count--;
    OS_MutexUnlock(q->mutex);
    free(node);
    return data;
}

static inline void Queue_Destroy(Queue *q) {
    if (!q) return;
    // Note: This doesn't free the data in nodes, just the nodes themselves
    while (q->head) {
        Node *next = q->head->next;
        free(q->head);
        q->head = next;
    }
    OS_MutexDestroy(q->mutex);
    OS_SemaphoreDestroy(q->sem);
    free(q);
}

#endif // HARMONY_QUEUE_H
