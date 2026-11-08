/*
 * instructions.c - Per-project instructions (BORIS.md / AGENTS.md).
 *
 * See instructions.h. The file is read once at startup and folded into the
 * system prompt; nothing here runs during a conversation.
 */

#define _POSIX_C_SOURCE 200809L

#include "instructions.h"
#include "logger.h"
#include "sandbox.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char TRUNCATED_NOTE[] =
	"\n\n[The instructions file is longer than 32 KB; the rest was not loaded.]";

/* Read up to INSTRUCTIONS_MAX_BYTES of a regular text file. */
static char *read_capped(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return NULL;

	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	char *buf = malloc(INSTRUCTIONS_MAX_BYTES + sizeof(TRUNCATED_NOTE) + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t n = fread(buf, 1, INSTRUCTIONS_MAX_BYTES, f);
	bool more = fgetc(f) != EOF;
	fclose(f);

	if (memchr(buf, '\0', n)) {
		log_warning("instructions: %s looks binary; ignored", path);
		free(buf);
		return NULL;
	}
	if (more) {
		while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80)
			n--; /* do not end inside a UTF-8 character */
		memcpy(buf + n, TRUNCATED_NOTE, sizeof(TRUNCATED_NOTE));
		log_warning("instructions: %s is over %d bytes; truncated", path,
			    INSTRUCTIONS_MAX_BYTES);
	} else {
		buf[n] = '\0';
	}

	/* An empty (or whitespace-only) file means no instructions. */
	const char *p = buf;
	while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
		p++;
	if (!*p) {
		free(buf);
		return NULL;
	}
	return buf;
}

char *instructions_load(const struct agent_configuration *cfg, char *source,
			size_t source_size)
{
	if (source && source_size)
		source[0] = '\0';
	if (!cfg)
		return NULL;

	const char *choice = cfg->instructions_file;
	if (choice && (strcmp(choice, "none") == 0 || strcmp(choice, "off") == 0))
		return NULL;

	/* An explicit path: the user's choice, wherever it is. */
	if (choice && choice[0] && strcmp(choice, "auto") != 0) {
		char *text = read_capped(choice);
		if (!text)
			log_warning("instructions: cannot read %s", choice);
		else if (source)
			snprintf(source, source_size, "%s", choice);
		return text;
	}

	/* Auto: a file at the sandbox root, really inside the sandbox. */
	if (!cfg->sandbox_root || !cfg->sandbox_root[0])
		return NULL;
	static const char *const names[] = INSTRUCTIONS_AUTO_NAMES;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char candidate[PATH_MAX];
		snprintf(candidate, sizeof(candidate), "%s/%s", cfg->sandbox_root, names[i]);
		struct stat lst;
		if (lstat(candidate, &lst) != 0)
			continue;
		char resolved[PATH_MAX];
		if (sandbox_resolve(cfg->sandbox_root, names[i], resolved, sizeof(resolved)) != 0) {
			log_warning("instructions: %s points outside the sandbox; ignored", names[i]);
			continue;
		}
		char *text = read_capped(resolved);
		if (text) {
			if (source)
				snprintf(source, source_size, "%s", names[i]);
			log_info("instructions: loaded %s (%zu bytes)", names[i], strlen(text));
			return text;
		}
	}
	return NULL;
}

char *instructions_compose(const char *system_prompt, const char *instructions,
			   const char *source)
{
	static const char HEADING[] =
		"\n\n# Project instructions (from %s)\n\n"
		"The user keeps these standing instructions for this project in %s. "
		"Follow them unless the user tells you otherwise in the "
		"conversation.\n\n";
	if (!instructions)
		return system_prompt ? strdup(system_prompt) : NULL;
	const char *base = system_prompt ? system_prompt : "";
	const char *name = source && source[0] ? source : "the project";
	size_t size = strlen(base) + sizeof(HEADING) + 2 * strlen(name) + strlen(instructions) + 1;
	char *out = malloc(size);
	if (!out)
		return NULL;
	int n = snprintf(out, size, "%s", base);
	n += snprintf(out + n, size - (size_t)n, HEADING, name, name);
	snprintf(out + n, size - (size_t)n, "%s", instructions);
	return out;
}
