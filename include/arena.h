/*
 * arena.h - Region-based memory allocator.
 *
 * Allocations are served bump-pointer style from fixed-size blocks.
 * arena_reset() reclaims all memory in O(1). Individual allocations
 * cannot be freed — the arena is released as a whole.
 *
 * Useful for temporary per-turn scratch allocations: allocate freely,
 * then reset in one call when the turn is done.
 */

#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

/* Linked block within an arena. */
struct memory_block {
	struct memory_block *next_block;
	size_t total_size;
	size_t used_bytes;
	unsigned char data[];
};

/* Arena handle. block_size controls the granularity of OS allocations. */
struct memory_arena {
	struct memory_block *first_block;
	struct memory_block *current_block;
	size_t block_size;
};

/* Snapshot for partial rollback via arena_restore(). */
struct arena_checkpoint {
	struct memory_block *block_at_save;
	size_t used_bytes_at_save;
};

/* Create a new arena. block_size is the allocation granularity. */
struct memory_arena *arena_create(size_t block_size);

/* Destroy the arena and free all backing memory. */
void arena_destroy(struct memory_arena *arena);

/* Rewind all blocks to empty; no OS frees are performed. */
void arena_reset(struct memory_arena *arena);

/* Allocate bytes from the arena. Returns NULL on OOM. */
void *arena_allocate(struct memory_arena *arena, size_t bytes);

/* Allocate zeroed memory from the arena. */
void *arena_allocate_zeroed(struct memory_arena *arena, size_t bytes);

/* Duplicate a NUL-terminated string into arena memory. */
char *arena_duplicate_string(struct memory_arena *arena, const char *string);

/* Duplicate up to length characters of a string into arena memory. */
char *arena_duplicate_string_length(struct memory_arena *arena, const char *string, size_t length);

/* Capture the current allocation watermark. */
struct arena_checkpoint arena_save(struct memory_arena *arena);

/* Roll back to a previously captured watermark. */
void arena_restore(struct memory_arena *arena, struct arena_checkpoint checkpoint);

#endif /* ARENA_H */
