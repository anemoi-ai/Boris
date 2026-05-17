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
#include "sandbox.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>
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
 * Get the home directory of the current user.
 */
static const char *get_home_dir(void)
{
	const char *home = getenv("HOME");
	if (home && home[0])
		return home;

	struct passwd *pw = getpwuid(getuid());
	if (pw && pw->pw_dir && pw->pw_dir[0])
		return pw->pw_dir;

	return "/";
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
	char boris_dir[PATH_MAX];
	char config_path[PATH_MAX + 64];
	const char *home;
	FILE *config_file;
	time_t now;
	struct tm *time_info;
	char date_str[64];
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
	if (!ask("What should I call myself? (this becomes your workspace folder) ",
		 companion_name, sizeof(companion_name))) {
		printf("\n  Setup cancelled.\n\n");
		return 1;
	}

	if (companion_name[0] == '\0') {
		snprintf(companion_name, sizeof(companion_name), "boris-companion");
	}

	/* Sanitize the companion name for use as a folder name */
	for (size_t i = 0; companion_name[i]; i++) {
		char c = companion_name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '-' || c == '_')) {
			companion_name[i] = '-';
		}
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
		snprintf(personality, sizeof(personality),
			 "You are a helpful AI companion. You are patient, curious, "
			 "and enjoy learning alongside your human. You explain things "
			 "clearly and encourage exploration.");
	}

	printf("\n");

	/* ------------------------------------------------------------------ */
	/* Save the configuration                                               */
	/* ------------------------------------------------------------------ */

	home = get_home_dir();
	if (!home) {
		fprintf(stderr, "  Could not determine home directory.\n");
		return 1;
	}

	snprintf(boris_dir, sizeof(boris_dir), "%s/.boris", home);
	snprintf(config_path, sizeof(config_path), "%s/config.ini", boris_dir);

	if (sandbox_mkdirp(boris_dir, 0755) != 0) {
		fprintf(stderr, "  Could not create %s\n", boris_dir);
		return 1;
	}

	/*
	 * Write the personality to a separate file so it is not subject to
	 * the INI parser's 1024-character line limit and is easy to edit.
	 */
	char prompt_path[PATH_MAX + 64];
	snprintf(prompt_path, sizeof(prompt_path), "%s/system_prompt.txt", boris_dir);
	int wrote_prompt_file = 0;
	FILE *prompt_file = fopen(prompt_path, "w");
	if (prompt_file) {
		fprintf(prompt_file, "%s\n", personality);
		fprintf(prompt_file, "The user's name is %s.\n", user_name);
		fclose(prompt_file);
		wrote_prompt_file = 1;
	}

	config_file = fopen(config_path, "w");
	if (!config_file) {
		fprintf(stderr, "  Could not write config to %s\n", config_path);
		return 1;
	}

	time(&now);
	time_info = localtime(&now);
	strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", time_info);

	fprintf(config_file, "# %s's configuration\n", companion_name);
	fprintf(config_file, "# Created on %s\n", date_str);
	fprintf(config_file, "# You are %s. This is who I am.\n", user_name);
	fprintf(config_file, "#\n");
	fprintf(config_file, "# Change anything here. This is yours.\n");
	fprintf(config_file, "\n");
	fprintf(config_file, "[model]\n");
	fprintf(config_file, "endpoint = %s\n", endpoint);
	fprintf(config_file, "name = %s\n", model_name);
	if (api_key[0] != '\0') {
		fprintf(config_file, "api_key = %s\n", api_key);
	}
	fprintf(config_file, "temperature = 0.7\n");
	fprintf(config_file, "max_tokens = 32768\n");
	fprintf(config_file, "\n");
	fprintf(config_file, "[prompt]\n");
	if (wrote_prompt_file) {
		fprintf(config_file, "system_prompt_file = %s/system_prompt.txt\n",
			boris_dir);
	} else {
		fprintf(config_file, "system_prompt = %s\n", personality);
	}
	fprintf(config_file, "\n");
	fprintf(config_file, "[agent]\n");
	fprintf(config_file, "max_iterations = 16\n");
	fprintf(config_file, "\n");
	fprintf(config_file, "[logging]\n");
	fprintf(config_file, "level = info\n");
	fprintf(config_file, "\n");
	fprintf(config_file, "[behaviour]\n");
	fprintf(config_file, "confirm_writes = false\n");
	fprintf(config_file, "\n");
	fprintf(config_file, "[tools]\n");
	fprintf(config_file, "enabled = read,write,list_dir,memory,run\n");
	fprintf(config_file, "sandbox_root = %s/sandbox\n", boris_dir);
	fprintf(config_file, "memory_persist = true\n");
	fprintf(config_file, "run_timeout = 30\n");

	fclose(config_file);
	chmod(config_path, 0600);

	printf("  Configuration saved to %s\n", config_path);

	char sandbox_path[PATH_MAX + 16];
	snprintf(sandbox_path, sizeof(sandbox_path), "%s/sandbox", boris_dir);
	if (sandbox_mkdirp(sandbox_path, 0755) != 0) {
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
	printf("  Run './build/boris chat' to start chatting.\n");
	printf("\n");
	printf("  See you there.\n");
	printf("\n");

	return 0;
}
