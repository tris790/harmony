#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include <pipewire/pipewire.h>

static atomic_bool stopping;
static void StopPlaybackLoop(struct pw_thread_loop *loop);

// Let a pending callback attempt its mutex lock exactly when shutdown joins it.
#define pw_thread_loop_stop StopPlaybackLoop
#include "../src/platform/audio_pipewire.c"
#undef pw_thread_loop_stop

static void StopPlaybackLoop(struct pw_thread_loop *loop) {
    atomic_store(&stopping, true);
    pw_thread_loop_stop(loop);
}

static void TestArenaAlignment(void) {
    MemoryArena arena;
    ArenaInit(&arena, 4096);

    for (size_t prefix_size = 1; prefix_size <= 2 * _Alignof(max_align_t); prefix_size++) {
        ArenaClear(&arena);
        uint8_t *prefix = ArenaPush(&arena, prefix_size);
        memset(prefix, 0xA5, prefix_size);
        TemporaryMemory temp = BeginTemporaryMemory(&arena);

        AudioPlaybackContext *ctx = PushStructZero(&arena, AudioPlaybackContext);
        assert((uintptr_t)ctx % _Alignof(AudioPlaybackContext) == 0);
        assert((uintptr_t)&ctx->mutex % _Alignof(pthread_mutex_t) == 0);
        for (size_t i = 0; i < sizeof(*ctx); i++) {
            assert(((uint8_t *)ctx)[i] == 0);
        }
        for (size_t i = 0; i < prefix_size; i++) assert(prefix[i] == 0xA5);

        ArenaPush(&arena, 3);
        uint64_t *values = PushArray(&arena, 2, uint64_t);
        assert((uintptr_t)values % _Alignof(uint64_t) == 0);
        values[0] = 123;
        values[1] = 456;
        ArenaPush(&arena, 5);
        void *raw = ArenaPush(&arena, sizeof(max_align_t));
        assert((uintptr_t)raw % _Alignof(max_align_t) == 0);

        EndTemporaryMemory(temp);
        assert(arena.used == temp.used_checkpoint);
        assert(PushStruct(&arena, AudioPlaybackContext) == ctx);
    }

    // Alignment padding counts against capacity, including an exact-fit allocation.
    ArenaClear(&arena);
    ArenaPush(&arena, 1);
    size_t aligned_offset = _Alignof(max_align_t);
    ArenaPush(&arena, arena.size - aligned_offset);
    assert(arena.used == arena.size);
    munmap(arena.base, arena.size);
}

static atomic_bool attempting_lock;

static void *ContendPlaybackMutex(void *data) {
    AudioPlaybackContext *ctx = data;
    atomic_store(&attempting_lock, true);
    assert(pthread_mutex_lock(&ctx->mutex) == 0);
    assert(pthread_mutex_unlock(&ctx->mutex) == 0);
    return NULL;
}

static void TestContendedPlaybackMutex(void) {
    MemoryArena arena;
    ArenaInit(&arena, 4096);
    ArenaPush(&arena, 1); // Simulate variable-length clipboard data before playback.
    AudioPlaybackContext *ctx = PushStructZero(&arena, AudioPlaybackContext);
    assert(pthread_mutex_init(&ctx->mutex, NULL) == 0);
    assert(pthread_mutex_lock(&ctx->mutex) == 0);

    pthread_t worker;
    assert(pthread_create(&worker, NULL, ContendPlaybackMutex, ctx) == 0);
    while (!atomic_load(&attempting_lock)) usleep(1000);
    usleep(20000); // Give the contender time to enter the futex wait path.
    assert(pthread_mutex_unlock(&ctx->mutex) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(pthread_mutex_destroy(&ctx->mutex) == 0);
    munmap(arena.base, arena.size);
}

static atomic_bool callback_started;
static atomic_bool callback_finished;

static void ShutdownCallback(void *data, uint64_t count) {
    (void)count;
    AudioPlaybackContext *ctx = data;
    atomic_store(&callback_started, true);
    while (!atomic_load(&stopping)) usleep(1000);
    assert(pthread_mutex_lock(&ctx->mutex) == 0);
    assert(pthread_mutex_unlock(&ctx->mutex) == 0);
    atomic_store(&callback_finished, true);
}

static void TestPlaybackShutdown(void) {
    MemoryArena arena;
    ArenaInit(&arena, 4096);
    AudioPlaybackContext *ctx = PushStructZero(&arena, AudioPlaybackContext);
    assert(pthread_mutex_init(&ctx->mutex, NULL) == 0);
    ctx->thread_loop = pw_thread_loop_new("Playback shutdown test", NULL);
    assert(ctx->thread_loop);

    struct pw_loop *loop = pw_thread_loop_get_loop(ctx->thread_loop);
    struct spa_source *event = pw_loop_add_event(loop, ShutdownCallback, ctx);
    assert(event);
    assert(pw_thread_loop_start(ctx->thread_loop) == 0);
    assert(pw_loop_signal_event(loop, event) == 0);
    while (!atomic_load(&callback_started)) usleep(1000);

    // This deadlocks if close holds the mutex while joining the callback thread.
    Audio_ClosePlayback(ctx);
    assert(atomic_load(&callback_finished));
    munmap(arena.base, arena.size);
}

int main(void) {
    alarm(10); // Fail promptly if a synchronization regression deadlocks.
    TestArenaAlignment();
    TestContendedPlaybackMutex();
    pw_init(NULL, NULL);
    TestPlaybackShutdown();
    pw_deinit();
    alarm(0);
    puts("Audio playback tests passed (alignment, contended mutex, shutdown).");
    return 0;
}
