/*
 * arena.c - Region-based memory allocator.
 *
 * Allocations are served bump-pointer style from fixed-size blocks. When a
 * block fills, a new one is linked in. All returned pointers are 16-byte
 * aligned.
 *
 * arena_reset() reclaims all memory in O(1) by rewinding the first block and
 * freeing any overflow blocks — no per-allocation bookkeeping required.
 * Used in the agent loop to reclaim per-iteration temporaries in one call.
 */

#include "arena.h"
#include <stdlib.h>
#include <string.h>

/*
 * Returns the start of the usable data region within a block, aligned to
 * 16 bytes. The struct header may not be a multiple of 16, so the offset
 * is computed at runtime.
 */
static unsigned char *block_data_start(struct memory_block *block)
{
	unsigned char *raw_start = (unsigned char *)(block + 1);
	return (unsigned char *)(((size_t)raw_start + 15) & ~(size_t)15);
}

static struct memory_block *arena_create_block(struct memory_arena *arena)
{
	struct memory_block *block;
	size_t header_size;
	size_t aligned_start;
	size_t total_size;

	/*
	 * Memory layout per block:
	 *   [struct header | up to 15 bytes padding | 16-byte-aligned data area]
	 */
	header_size = sizeof(struct memory_block);
	aligned_start = ((header_size + 15) & ~(size_t)15);
	total_size = aligned_start + arena->block_size;

	block = malloc(total_size);
	if (!block)
		return NULL;

	block->next_block = NULL;
	block->total_size = arena->block_size;
	block->used_bytes = 0;

	return block;
}

struct memory_arena *arena_create(size_t block_size)
{
	struct memory_arena *arena;

	if (block_size == 0)
		return NULL;

	arena = malloc(sizeof(struct memory_arena));
	if (!arena)
		return NULL;

	arena->block_size = block_size;
	arena->first_block = NULL;
	arena->current_block = NULL;

	arena->first_block = arena_create_block(arena);
	if (!arena->first_block) {
		free(arena);
		return NULL;
	}

	arena->current_block = arena->first_block;

	return arena;
}

void arena_destroy(struct memory_arena *arena)
{
	struct memory_block *block;
	struct memory_block *next;

	if (!arena)
		return;

	block = arena->first_block;
	while (block) {
		next = block->next_block;
		free(block);
		block = next;
	}

	free(arena);
}

/*
 * Reset rewinds to the first block and frees any overflow blocks.
 * O(1) in the common case (no overflow); O(n) only if blocks were chained.
 */
void arena_reset(struct memory_arena *arena)
{
	if (!arena)
		return;

	if (arena->first_block) {
		struct memory_block *block = arena->first_block->next_block;
		while (block) {
			struct memory_block *next = block->next_block;
			free(block);
			block = next;
		}

		arena->first_block->used_bytes = 0;
		arena->first_block->next_block = NULL;
		arena->current_block = arena->first_block;
	}
}

void *arena_allocate(struct memory_arena *arena, size_t bytes)
{
	/* Round up to 16-byte alignment: (bytes + 15) & ~15 */
	size_t aligned_bytes = (bytes + 15) & ~(size_t)15;

	if (!arena || bytes == 0)
		return NULL;

	if (arena->current_block->used_bytes + aligned_bytes > arena->current_block->total_size) {
		struct memory_block *new_block = arena_create_block(arena);
		if (!new_block)
			return NULL;

		arena->current_block->next_block = new_block;
		arena->current_block = new_block;
	}

	unsigned char *data_start = block_data_start(arena->current_block);
	void *ptr = data_start + arena->current_block->used_bytes;
	arena->current_block->used_bytes += aligned_bytes;

	return ptr;
}

void *arena_allocate_zeroed(struct memory_arena *arena, size_t bytes)
{
	void *ptr = arena_allocate(arena, bytes);
	if (ptr)
		memset(ptr, 0, bytes);
	return ptr;
}

char *arena_duplicate_string(struct memory_arena *arena, const char *string)
{
	if (!string)
		return NULL;

	size_t length = strlen(string);
	char *copy = arena_allocate(arena, length + 1);
	if (copy)
		memcpy(copy, string, length + 1);
	return copy;
}

char *arena_duplicate_string_length(struct memory_arena *arena, const char *string, size_t length)
{
	if (!string)
		return NULL;

	char *copy = arena_allocate(arena, length + 1);
	if (copy) {
		memcpy(copy, string, length);
		copy[length] = '\0';
	}
	return copy;
}

struct arena_checkpoint arena_save(struct memory_arena *arena)
{
	struct arena_checkpoint checkpoint;

	if (!arena) {
		memset(&checkpoint, 0, sizeof(checkpoint));
		return checkpoint;
	}

	checkpoint.block_at_save = arena->current_block;
	checkpoint.used_bytes_at_save = arena->current_block->used_bytes;

	return checkpoint;
}

/*
 * Restores the arena to a saved checkpoint. Any blocks allocated after the
 * checkpoint are freed to avoid leaking them as orphans.
 */
void arena_restore(struct memory_arena *arena, struct arena_checkpoint checkpoint)
{
	if (!arena)
		return;

	arena->current_block = checkpoint.block_at_save;
	arena->current_block->used_bytes = checkpoint.used_bytes_at_save;

	struct memory_block *orphan = arena->current_block->next_block;
	arena->current_block->next_block = NULL;
	while (orphan) {
		struct memory_block *next = orphan->next_block;
		free(orphan);
		orphan = next;
	}
}
