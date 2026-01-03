#include "../os_api.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdatomic.h>

struct OS_Thread {
    pthread_t handle;
};

OS_Thread* OS_ThreadCreate(OS_ThreadProc proc, void *data) {
    OS_Thread *thread = (OS_Thread *)malloc(sizeof(OS_Thread));
    if (pthread_create(&thread->handle, NULL, (void* (*)(void*))proc, data) != 0) {
        free(thread);
        return NULL;
    }
    return thread;
}

void OS_ThreadJoin(OS_Thread *thread) {
    if (thread) {
        pthread_join(thread->handle, NULL);
        free(thread);
    }
}

void OS_ThreadDetach(OS_Thread *thread) {
    if (thread) {
        pthread_detach(thread->handle);
        free(thread);
    }
}

struct OS_Mutex {
    pthread_mutex_t handle;
};

OS_Mutex* OS_MutexCreate(void) {
    OS_Mutex *mutex = (OS_Mutex *)malloc(sizeof(OS_Mutex));
    pthread_mutex_init(&mutex->handle, NULL);
    return mutex;
}

void OS_MutexLock(OS_Mutex *mutex) {
    if (mutex) pthread_mutex_lock(&mutex->handle);
}

void OS_MutexUnlock(OS_Mutex *mutex) {
    if (mutex) pthread_mutex_unlock(&mutex->handle);
}

void OS_MutexDestroy(OS_Mutex *mutex) {
    if (mutex) {
        pthread_mutex_destroy(&mutex->handle);
        free(mutex);
    }
}

struct OS_Semaphore {
    sem_t handle;
};

OS_Semaphore* OS_SemaphoreCreate(uint32_t initial_value) {
    OS_Semaphore *sem = (OS_Semaphore *)malloc(sizeof(OS_Semaphore));
    sem_init(&sem->handle, 0, initial_value);
    return sem;
}

void OS_SemaphoreWait(OS_Semaphore *sem) {
    if (sem) sem_wait(&sem->handle);
}

void OS_SemaphorePost(OS_Semaphore *sem) {
    if (sem) sem_post(&sem->handle);
}

void OS_SemaphoreDestroy(OS_Semaphore *sem) {
    if (sem) {
        sem_destroy(&sem->handle);
        free(sem);
    }
}

int32_t OS_AtomicIncrement(int32_t *val) {
    return atomic_fetch_add((_Atomic int32_t *)val, 1) + 1;
}

int32_t OS_AtomicDecrement(int32_t *val) {
    return atomic_fetch_sub((_Atomic int32_t *)val, 1) - 1;
}
