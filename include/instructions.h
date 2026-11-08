/*
 * instructions.h - Per-project instructions (BORIS.md / AGENTS.md).
 *
 * A project can keep standing instructions for the agent in a file at the
 * root of the sandbox: BORIS.md, or AGENTS.md (the name several agents
 * share). Boris reads it at startup and appends it to the system prompt,
 * so it applies to every turn and keeps the prompt prefix stable.
 *
 * prompt.instructions selects the file:
 *   auto (default)  BORIS.md, else AGENTS.md, at the sandbox root
 *   none            no project instructions
 *   PATH            this file (anywhere; the user chose it)
 *
 * In auto mode the file must really be inside the sandbox: a symlink
 * pointing elsewhere is refused, so a link planted by a script cannot
 * pull a private file into the prompt.
 */

#ifndef INSTRUCTIONS_H
#define INSTRUCTIONS_H

#include <stddef.h>
#include "boris_types.h"

/* Larger files are cut at this size, with a note. */
#define INSTRUCTIONS_MAX_BYTES (32 * 1024)

/* File names tried at the sandbox root in auto mode, in order. */
#define INSTRUCTIONS_AUTO_NAMES {"BORIS.md", "AGENTS.md"}

/*
 * Load the project instructions selected by cfg. Returns the text (heap,
 * caller frees) or NULL if there are none. *source gets the name to show
 * the user ("BORIS.md", or the configured path).
 */
char *instructions_load(const struct agent_configuration *cfg, char *source,
			size_t source_size);

/*
 * The system prompt with the instructions appended under a heading that
 * names their source. Returns a heap string (caller frees), or NULL.
 */
char *instructions_compose(const char *system_prompt, const char *instructions,
			   const char *source);

#endif /* INSTRUCTIONS_H */
