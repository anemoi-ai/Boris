/*
 * init.c - First-run setup wizard.
 *
 * Prompts for user name, companion name, model endpoint, model name,
 * API key, and personality prompt, then writes ~/.boris/config.ini and
 * ~/.boris/system_prompt.txt. Creates the sandbox directory and probes
 * the configured endpoint before exiting.
 */

#define _GNU_SOURCE

#include "init.h"
#include "configuration.h"
#include "fs.h"
#include <curl/curl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>

#define MAX_USER_INPUT 1024

/*
 * Read a line of input from the user, stripping the newline.
 *
 * prompt : What to show the user
 * buffer : Where to store the input
 * size   : Size of the buffer
 *
 * Returns the buffer, or NULL on EOF.
 */
static char *ask(const char *prompt, char *buffer, size_t size)
{
	printf("  %s", prompt);
	fflush(stdout);

	if (!fgets(buffer, (int)size, stdin))
		return NULL;

	/* Strip newline */
	size_t len = strlen(buffer);
	while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
		buffer[len - 1] = '\0';
		len--;
	}

	/* Strip trailing spaces */
	while (len > 0 && buffer[len - 1] == ' ') {
		buffer[len - 1] = '\0';
		len--;
	}

	return buffer;
}

/*
 * Read input without echoing (for sensitive info like API keys).
 */
static char *ask_hidden(const char *prompt, char *buffer, size_t size)
{
	printf("  %s", prompt);
	fflush(stdout);

	struct termios old, noecho;
	int tty = isatty(STDIN_FILENO);
	if (tty) {
		tcgetattr(STDIN_FILENO, &old);
		noecho = old;
		noecho.c_lflag &= ~(tcflag_t)ECHO;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &noecho);
	}

	char *result = fgets(buffer, (int)size, stdin);

	if (tty) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
		printf("\n");
	}

	if (!result)
		return NULL;

	size_t len = strlen(buffer);
	while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r'))
		buffer[--len] = '\0';

	return buffer;
}

static size_t discard_write_cb(void *ptr, size_t size, size_t nmemb,
			       void *userdata)
{
	(void)ptr;
	(void)userdata;
	return size * nmemb;
}

/*
 * Try to reach the configured endpoint and report the result.
 * Always returns - a failed probe does not abort setup.
 */
static void probe_endpoint(const char *url)
{
	printf("  Checking connection to %s... ", url);
	fflush(stdout);

	CURL *curl = curl_easy_init();
	if (!curl) {
		printf("skipped\n");
		return;
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write_cb);
	curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res == CURLE_OK) {
		printf("OK\n");
	} else {
		printf("failed (%s)\n", curl_easy_strerror(res));
		printf("  Configuration saved. Check your endpoint before chatting.\n");
	}
}

/*
 * Write config.ini into buf. Returns the length, or -1 if it did not fit.
 */
static int format_config(char *buf, size_t size, const char *companion_name,
			 const char *user_name, const char *endpoint,
			 const char *model_name, const char *api_key,
			 const char *prompt_path, const char *personality,
			 const char *boris_dir)
{
	char date_str[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", &tm);

	char api_key_line[MAX_USER_INPUT + 16] = "";
	if (api_key[0])
		snprintf(api_key_line, sizeof(api_key_line), "api_key = %s\n", api_key);

	char prompt_line[MAX_USER_INPUT * 4 + 64];
	if (prompt_path)
		snprintf(prompt_line, sizeof(prompt_line),
			 "system_prompt_file = %s\n", prompt_path);
	else
		snprintf(prompt_line, sizeof(prompt_line),
			 "system_prompt = %s\n", personality);

	int n = snprintf(buf, size,
			 "# %s's configuration\n"
			 "# Created on %s\n"
			 "# You are %s. This is who I am.\n"
			 "#\n"
			 "# Change anything here. This is yours.\n"
			 "\n"
			 "[model]\n"
			 "endpoint = %s\n"
			 "name = %s\n"
			 "%s"
			 "temperature = 0.7\n"
			 "# context_window must match your model's context length\n"
			 "# (llama.cpp -c, Ollama num_ctx); max_tokens must be well below it.\n"
			 "context_window = 32768\n"
			 "max_tokens = 8192\n"
			 "\n"
			 "[prompt]\n"
			 "%s"
			 "\n"
			 "[agent]\n"
			 "max_iterations = 16\n"
			 "\n"
			 "[logging]\n"
			 "level = info\n"
			 "\n"
			 "[behaviour]\n"
			 "# Ask before each write / script run\n"
			 "confirm_writes = false\n"
			 "confirm_run = false\n"
			 "\n"
			 "[tools]\n"
			 "# default = every tool except fetch; see boris.ini.example\n"
			 "enabled = default\n"
			 "sandbox_root = %s/sandbox\n"
			 "memory_persist = true\n"
			 "run_timeout = 30\n",
			 companion_name, date_str, user_name, endpoint, model_name,
			 api_key_line, prompt_line, boris_dir);
	return (n < 0 || (size_t)n >= size) ? -1 : n;
}

/*
 * Run the interactive setup wizard.
 */
int init_run(void)
{
	char companion_name[MAX_USER_INPUT];
	char user_name[MAX_USER_INPUT];
	char endpoint[MAX_USER_INPUT];
	char model_name[MAX_USER_INPUT];
	char personality[MAX_USER_INPUT * 4];
	char api_key[MAX_USER_INPUT];

	printf("\n");
	printf("  ========================================\n");
	printf("   Boris - First Setup\n");
	printf("  ========================================\n");
	printf("\n");
	printf("  Let's get to know each other. I'll ask a few questions,\n");
	printf("  and together we'll create your personal AI companion.\n");
	printf("\n");

	/* Question 1: User's name */
	if (!ask("What should I call you? ", user_name, sizeof(user_name))) {
		printf("\n  Setup cancelled.\n\n");
		return 1;
	}

	if (user_name[0] == '\0') {
		snprintf(user_name, sizeof(user_name), "friend");
	}

	printf("  Nice to meet you, %s.\n\n", user_name);

	/* Question 2: Companion name */
	if (!ask("What should I call myself? ", companion_name, sizeof(companion_name))) {
		printf("\n  Setup cancelled.\n\n");
		return 1;
	}

	if (companion_name[0] == '\0') {
		snprintf(companion_name, sizeof(companion_name), "boris-companion");
	}

	printf("  I like the name %s. That's who I'll be.\n\n", companion_name);

	/* Question 3: Model endpoint */
	printf("  Where is your AI model running?\n");
	printf("  (For llama.cpp: http://localhost:8080/v1/chat/completions)\n");
	printf("  (For Ollama:    http://localhost:11434/v1/chat/completions)\n");
	printf("\n");

	if (!ask("Model endpoint URL: ", endpoint, sizeof(endpoint))) {
		printf("\n  Setup cancelled.\n\n");
		return 1;
	}

	if (endpoint[0] == '\0') {
		snprintf(endpoint, sizeof(endpoint),
			 "http://localhost:8080/v1/chat/completions");
	}

	printf("  Got it. I'll talk to the model at %s\n\n", endpoint);

	/* Question 4: Model name */
	if (!ask("What model are you running? (e.g., llama3, mistral) ",
		 model_name, sizeof(model_name))) {
		printf("\n  Setup cancelled.\n\n");
		return 1;
	}

	if (model_name[0] == '\0') {
		snprintf(model_name, sizeof(model_name), "default");
	}

	printf("  %s it is.\n\n", model_name);

	/* Question 5: API key (optional) */
	if (!ask_hidden("API key (optional, press Enter to skip): ",
			api_key, sizeof(api_key))) {
		printf("\n  Setup cancelled.\n\n");
		return 1;
	}

	/* Question 6: Personality */
	printf("\n");
	printf("  Now, who should I be? This is your chance to shape my\n");
	printf("  personality. You can change this later, so don't worry\n");
	printf("  about getting it perfect.\n");
	printf("\n");
	printf("  Examples:\n");
	printf("    - A helpful coding assistant who explains things clearly\n");
	printf("    - A curious companion who asks questions and learns with me\n");
	printf("    - A patient tutor who never gets frustrated\n");
	printf("\n");

	if (!ask("Who should I be? ", personality, sizeof(personality))) {
		printf("\n  Setup cancelled.\n\n");
		return 1;
	}

	if (personality[0] == '\0') {
		snprintf(personality, sizeof(personality), "%s",
			 BORIS_DEFAULT_SYSTEM_PROMPT);
	}

	printf("\n");

	/* ------------------------------------------------------------------ */
	/* Save the configuration                                               */
	/* ------------------------------------------------------------------ */

	char *boris_dir = fs_boris_path(NULL);
	char *config_path = fs_boris_path("config.ini");
	char *prompt_path = fs_boris_path("system_prompt.txt");
	char *sandbox_path = fs_boris_path("sandbox");
	int rc = 1;

	if (!boris_dir || !config_path || !prompt_path || !sandbox_path) {
		fprintf(stderr, "  Could not determine home directory.\n");
		goto out;
	}

	if (fs_mkdirp(boris_dir, 0755) != 0) {
		fprintf(stderr, "  Could not create %s\n", boris_dir);
		goto out;
	}

	/*
	 * Write the personality to a separate file so it is not subject to
	 * the INI parser's line limit and is easy to edit.
	 */
	char prompt_text[MAX_USER_INPUT * 6];
	int plen = snprintf(prompt_text, sizeof(prompt_text),
			    "%s\nThe user's name is %s.\n", personality, user_name);
	bool wrote_prompt_file =
		plen > 0 && (size_t)plen < sizeof(prompt_text) &&
		fs_write_atomic(prompt_path, prompt_text, (size_t)plen, 0644, false) == 0;

	char config_text[16384];
	int clen = format_config(config_text, sizeof(config_text), companion_name,
				 user_name, endpoint, model_name, api_key,
				 wrote_prompt_file ? prompt_path : NULL,
				 personality, boris_dir);

	/* 0600 from the moment it exists: it may hold the API key. */
	if (clen < 0 ||
	    fs_write_atomic(config_path, config_text, (size_t)clen, 0600, false) != 0 ||
	    chmod(config_path, 0600) != 0) {
		fprintf(stderr, "  Could not write config to %s\n", config_path);
		goto out;
	}

	printf("  Configuration saved to %s\n", config_path);

	if (fs_mkdirp(sandbox_path, 0755) != 0) {
		fprintf(stderr, "  Warning: could not create sandbox at %s\n",
			sandbox_path);
	} else {
		printf("  Sandbox created at %s\n", sandbox_path);
	}

	probe_endpoint(endpoint);

	/* ------------------------------------------------------------------ */
	/* Print the farewell message                                           */
	/* ------------------------------------------------------------------ */

	printf("\n");
	printf("  ========================================\n");
	printf("   Setup Complete\n");
	printf("  ========================================\n");
	printf("\n");
	printf("  %s, your companion %s is ready.\n", user_name, companion_name);
	printf("\n");
	printf("  Run './boris chat' to start chatting.\n");
	printf("\n");
	printf("  See you there.\n");
	printf("\n");
	rc = 0;

out:
	free(boris_dir);
	free(config_path);
	free(prompt_path);
	free(sandbox_path);
	return rc;
}
