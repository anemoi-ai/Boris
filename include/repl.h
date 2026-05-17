/*
 * repl.h - Interactive chat loop.
 *
 * Reads user input, dispatches slash commands, and drives the agent loop.
 * Slash commands: /quit, /exit, /clear, /help, /status, /tokens.
 * Multi-line input is terminated by a blank line.
 */

#ifndef REPL_H
#define REPL_H

#include "boris_types.h"
#include "conversation.h"

/*
 * Run the interactive chat loop until the user quits or EOF.
 * conv must already contain a system prompt message.
 */
void repl_run(struct conversation_history *conv,
	      const struct agent_configuration *config);

#endif /* REPL_H */
