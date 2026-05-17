/*
 * init.h - First-run setup wizard.
 *
 * Invoked by `boris init`. Prompts for user name, companion name, model
 * endpoint, model name, API key, and personality prompt, then writes
 * ~/.boris/config.ini, ~/.boris/system_prompt.txt, and creates the sandbox
 * directory.
 */

#ifndef INIT_H
#define INIT_H

/*
 * Run the interactive setup wizard.
 * Returns 0 on success, non-zero on failure or user cancellation.
 */
int init_run(void);

#endif /* INIT_H */
