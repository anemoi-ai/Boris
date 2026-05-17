/*
 * test_arena.c - Testing Boris's short-term memory.
 *
 * Tests are how you prove to yourself that your code works.
 * Each test below checks one specific behaviour of the arena.
 * If a test fails, you know exactly what to fix.
 *
 * The pattern is simple:
 *   1. Set up the situation
 *   2. Do the thing
 *   3. Check the result
 *   4. Report pass or fail
 *
 * We use a simple counter: tests_passed and tests_failed.
 * At the end, we print the totals. No fancy framework needed.
 */

#define _POSIX_C_SOURCE 200809L

#include "arena.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int tests_passed = 0;
static int tests_failed = 0;

/*
 * A helper macro that makes tests readable.
 *
 * ASSERT(condition) checks if the condition is true.
 * If it is, we count a pass. If not, we count a failure
 * and print what went wrong.
 *
 * The __FILE__ and __LINE__ are built-in macros that tell
 * us which file and line the assertion is on. This is
 * how you find the failing test quickly.
 */
#define ASSERT(condition)                                                               \
	do {                                                                            \
		if (condition) {                                                        \
			tests_passed++;                                                 \
		} else {                                                                \
			tests_failed++;                                                 \
			printf("  FAIL: %s:%d - %s\n", __FILE__, __LINE__, #condition); \
		}                                                                       \
	} while (0)

/*
 * Test: Creating an arena gives us a valid pointer.
 *
 * The simplest test. If we can't even create an arena,
 * nothing else will work.
 */
static void test_arena_create(void)
{
	printf("  Test: arena creation\n");

	struct memory_arena *arena = arena_create(65536);
	ASSERT(arena != NULL);
	ASSERT(arena->first_block != NULL);
	ASSERT(arena->current_block != NULL);
	ASSERT(arena->block_size == 65536);

	arena_destroy(arena);
}

/*
 * Test: Creating an arena with zero size fails gracefully.
 *
 * Good code handles bad input without crashing.
 */
static void test_arena_create_zero_size(void)
{
	printf("  Test: arena creation with zero size\n");

	struct memory_arena *arena = arena_create(0);
	ASSERT(arena == NULL);
}

/*
 * Test: Allocating from the arena gives us usable memory.
 *
 * We allocate some bytes, write to them, and read them back.
 * If the data survives, the arena is working.
 */
static void test_arena_allocate(void)
{
	printf("  Test: basic allocation\n");

	struct memory_arena *arena = arena_create(65536);

	void *ptr = arena_allocate(arena, 100);
	ASSERT(ptr != NULL);

	/* Write some data and verify it */
	memset(ptr, 'A', 100);
	unsigned char *bytes = (unsigned char *)ptr;
	ASSERT(bytes[0] == 'A');
	ASSERT(bytes[99] == 'A');

	arena_destroy(arena);
}

/*
 * Test: Allocations are 16-byte aligned.
 *
 * This matters for certain CPU instructions (SIMD) that require
 * aligned memory. The arena should guarantee this automatically.
 */
static void test_arena_alignment(void)
{
	printf("  Test: 16-byte alignment\n");

	struct memory_arena *arena = arena_create(65536);

	void *ptr1 = arena_allocate(arena, 1);
	void *ptr2 = arena_allocate(arena, 1);
	void *ptr3 = arena_allocate(arena, 7);

	/* Check that each pointer is a multiple of 16 */
	ASSERT(((size_t)ptr1 % 16) == 0);
	ASSERT(((size_t)ptr2 % 16) == 0);
	ASSERT(((size_t)ptr3 % 16) == 0);

	arena_destroy(arena);
}

/*
 * Test: arena_allocate_zeroed gives us zeroed memory.
 */
static void test_arena_allocate_zeroed(void)
{
	printf("  Test: zeroed allocation\n");

	struct memory_arena *arena = arena_create(65536);

	unsigned char *ptr = arena_allocate_zeroed(arena, 64);
	ASSERT(ptr != NULL);

	/* Every byte should be zero */
	for (int i = 0; i < 64; i++) {
		ASSERT(ptr[i] == 0);
	}

	arena_destroy(arena);
}

/*
 * Test: arena_duplicate_string copies strings correctly.
 */
static void test_arena_duplicate_string(void)
{
	printf("  Test: string duplication\n");

	struct memory_arena *arena = arena_create(65536);

	const char *original = "Hello, Boris!";
	char *copy = arena_duplicate_string(arena, original);

	ASSERT(copy != NULL);
	ASSERT(strcmp(copy, original) == 0);
	/* Make sure it's a real copy, not the same pointer */
	ASSERT(copy != original);

	arena_destroy(arena);
}

/*
 * Test: arena_duplicate_string handles NULL input.
 */
static void test_arena_duplicate_string_null(void)
{
	printf("  Test: string duplication with NULL\n");

	struct memory_arena *arena = arena_create(65536);

	char *copy = arena_duplicate_string(arena, NULL);
	ASSERT(copy == NULL);

	arena_destroy(arena);
}

/*
 * Test: Resetting the arena clears everything.
 *
 * After reset, new allocations should reuse the same memory.
 * The old data is still physically there, but we've forgotten
 * about it - it's as if the arena is brand new.
 */
static void test_arena_reset(void)
{
	printf("  Test: arena reset\n");

	struct memory_arena *arena = arena_create(65536);

	/* Allocate something */
	char *first = arena_duplicate_string(arena, "first allocation");
	ASSERT(first != NULL);

	/* Reset */
	arena_reset(arena);

	/* Allocate again - should reuse the same space */
	char *second = arena_duplicate_string(arena, "second");
	ASSERT(second != NULL);
	ASSERT(strcmp(second, "second") == 0);

	/* The first allocation's memory may have been overwritten */
	/* That's expected - reset means "forget everything" */

	arena_destroy(arena);
}

/*
 * Test: Save and restore (checkpoints).
 *
 * This is like a save point in a game. You save, do some work,
 * and restore to undo it all.
 */
static void test_arena_checkpoint(void)
{
	printf("  Test: save and restore checkpoint\n");

	struct memory_arena *arena = arena_create(65536);

	/* Allocate something permanent */
	char *permanent = arena_duplicate_string(arena, "permanent");

	/* Save the state */
	struct arena_checkpoint checkpoint = arena_save(arena);

	/* Allocate some temporary things */
	char *temporary = arena_duplicate_string(arena, "temporary");
	ASSERT(temporary != NULL);
	ASSERT(strcmp(temporary, "temporary") == 0);

	/* Restore to the checkpoint */
	arena_restore(arena, checkpoint);

	/* The permanent allocation should still be accessible */
	ASSERT(strcmp(permanent, "permanent") == 0);

	/* The temporary allocation is now "forgotten" */
	/* (The memory is still there, but the arena doesn't know about it) */

	arena_destroy(arena);
}

/*
 * Test: Allocating more than one block's worth of memory.
 *
 * When a block fills up, the arena should allocate a new one
 * and chain it to the old one. This test forces that behaviour
 * by using a tiny block size.
 */
static void test_arena_multiple_blocks(void)
{
	printf("  Test: multiple blocks (block growth)\n");

	/* Use a tiny block size to force multiple blocks */
	struct memory_arena *arena = arena_create(64);

	/* Allocate more than 64 bytes total */
	void *ptr1 = arena_allocate(arena, 32);
	void *ptr2 = arena_allocate(arena, 32);
	void *ptr3 = arena_allocate(arena, 32); /* This should trigger a new block */

	ASSERT(ptr1 != NULL);
	ASSERT(ptr2 != NULL);
	ASSERT(ptr3 != NULL);

	/* The pointers should be different (from different blocks or positions) */
	ASSERT(ptr1 != ptr2);
	ASSERT(ptr2 != ptr3);

	/* The arena should have more than one block now */
	ASSERT(arena->first_block->next_block != NULL);

	arena_destroy(arena);
}

/*
 * Test: Destroying a NULL arena doesn't crash.
 *
 * Good functions handle NULL input gracefully.
 */
static void test_arena_destroy_null(void)
{
	printf("  Test: destroy NULL arena\n");

	arena_destroy(NULL); /* Should not crash */
	tests_passed++;	     /* If we got here, it didn't crash */
}

/*
 * Run all the tests.
 */
int main(void)
{
	printf("\n=== Arena Tests ===\n\n");

	test_arena_create();
	test_arena_create_zero_size();
	test_arena_allocate();
	test_arena_alignment();
	test_arena_allocate_zeroed();
	test_arena_duplicate_string();
	test_arena_duplicate_string_null();
	test_arena_reset();
	test_arena_checkpoint();
	test_arena_multiple_blocks();
	test_arena_destroy_null();

	printf("\n--- Arena Results ---\n");
	printf("  Passed: %d\n", tests_passed);
	printf("  Failed: %d\n", tests_failed);
	printf("\n");

	return tests_failed > 0 ? 1 : 0;
}
