#ifndef HARMONY_MEMORY_ARENA_H
#define HARMONY_MEMORY_ARENA_H

#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct MemoryArena {
    uint8_t *base;
    size_t size;
    size_t used;
} MemoryArena;

static void ArenaInit(MemoryArena *arena, size_t size) {
    // Use mmap for large reliable allocation
    arena->base = (uint8_t *)mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(arena->base != MAP_FAILED);
    arena->size = size;
    arena->used = 0;
}

static void *ArenaPush(MemoryArena *arena, size_t size) {
    assert((arena->used + size) <= arena->size);
    void *result = arena->base + arena->used;
    arena->used += size;
    return result;
}

static void *ArenaPushZero(MemoryArena *arena, size_t size) {
    void *result = ArenaPush(arena, size);
    for (size_t i = 0; i < size; ++i) {
        ((uint8_t *)result)[i] = 0;
    }
    return result;
}

static void ArenaPop(MemoryArena *arena, size_t size) {
    assert(arena->used >= size);
    arena->used -= size;
}

static void ArenaClear(MemoryArena *arena) {
    arena->used = 0;
}

#define PushStruct(arena, type) (type *)ArenaPush(arena, sizeof(type))
#define PushArray(arena, count, type) (type *)ArenaPush(arena, (count) * sizeof(type))
#define PushStructZero(arena, type) (type *)ArenaPushZero(arena, sizeof(type))

// Temporary memory helper
typedef struct TemporaryMemory {
    MemoryArena *arena;
    size_t used_checkpoint;
} TemporaryMemory;

static TemporaryMemory BeginTemporaryMemory(MemoryArena *arena) {
    TemporaryMemory result;
    result.arena = arena;
    result.used_checkpoint = arena->used;
    return result;
}

static void EndTemporaryMemory(TemporaryMemory temp) {
    assert(temp.arena->used >= temp.used_checkpoint);
    temp.arena->used = temp.used_checkpoint;
}

#endif // HARMONY_MEMORY_ARENA_H
