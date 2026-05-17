/*
 * main.c - Entry point and subcommand dispatcher.
 *
 *   boris init    - Interactive first-run setup wizard
 *   boris chat    - Interactive REPL session
 *   boris status  - Print active configuration
 */
#define _GNU_SOURCE

#include <unistd.h>

#include "boris_errors.h"
#include "boris_types.h"
#include "arena.h"
#include "logger.h"
#include "configuration.h"
#include "conversation.h"
#include "agent.h"
#include "repl.h"
#include "init.h"
#include "tools.h"
#include "http_client.h"
#include "metrics.h"
#include "json.h"
#include "terminal.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>

static bool g_quiet = false;

#define BORIS_VERSION "1.0.0"

static void clear_screen(void)
{
	printf("\033[2J\033[H");
}

static void print_banner(void)
{
	const char *banner =
		"██████   ██████  ██████  ██ ███████\n"
		"██   ██ ██    ██ ██   ██ ██ ██\n"
		"██████  ██    ██ ██████  ██ ███████\n"
		"██   ██ ██    ██ ██   ██ ██      ██\n"
		"██████   ██████  ██   ██ ██ ███████\n"
		"\n";
	if (term_supports_color())
		printf("%s%s%s", TERM_FG_BRIGHT_GREEN, banner, TERM_RESET);
	else
		printf("%s", banner);
	printf("  Boris v%s - Your AI companion\n\n", BORIS_VERSION);
}

static void print_usage(void)
{
	printf("  Usage: boris <command> [options]\n");
	printf("\n");
	printf("  Commands:\n");
	printf("    init     Set up your first Boris companion\n");
	printf("    chat     Start an interactive conversation\n");
	printf("    status   Show your current configuration\n");
	printf("    help     Show this help message\n");
	printf("\n");
	printf("  Options (for chat):\n");
	printf("    --config FILE       Load configuration from FILE\n");
	printf("    --tools LIST        Comma-separated tools to enable\n");
	printf("                        (read,write,list_dir,memory,all,none)\n");
	printf("    --sandbox DIR       Set sandbox root directory\n");
	printf("    --max-iterations N  Max ReAct loop iterations (default: 16)\n");
	printf("    --max-retries N     Max retries on LLM errors (default: 2)\n");
	printf("    --stream            Enable streaming responses\n");
	printf("    --log-level LVL     Set log level (debug/info/warn/error)\n");
	printf("    --log-file FILE     Write logs to FILE\n");
	printf("    --show-metrics      Print metrics at exit\n");
	printf("    --insecure          Skip SSL certificate verification\n");
	printf("    --set KEY=VALUE     Override a config setting\n");
	printf("    --version           Show version\n");
	printf("\n");
	printf("  Getting started:\n");
	printf("    1. Run 'boris init' to set up your companion\n");
	printf("    2. Start your AI model (llama.cpp or Ollama)\n");
	printf("    3. Run 'boris chat' to talk\n");
	printf("\n");
	printf("  Examples:\n");
	printf("    boris chat --tools read,write,list_dir,memory\n");
	printf("    boris chat --sandbox ./workspace --max-iterations 8\n");
	printf("    boris chat --set model.temperature=0.3\n");
	printf("\n");
}

static void print_version(void)
{
	printf("  Boris v%s\n", BORIS_VERSION);
}

static void configuration_load_standard(struct agent_configuration *config)
{
	configuration_set_defaults(config);
	const char *home = getenv("HOME");
	if (home) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/.boris/config.ini", home);
		configuration_load_from_file(config, path);
	}
	configuration_apply_environment(config);
}

static int cmd_chat(int argc, char *argv[])
{
	struct agent_configuration config;
	struct conversation_history *conv;
	const char *system_prompt;

	configuration_load_standard(&config);

	/* Parse CLI options with getopt_long */
	static struct option long_opts[] = {
		{"config", required_argument, 0, 'c'},
		{"tools", required_argument, 0, 't'},
		{"sandbox", required_argument, 0, 's'},
		{"max-iterations", required_argument, 0, 'i'},
		{"max-retries", required_argument, 0, 'r'},
		{"stream", no_argument, 0, 'S'},
		{"log-level", required_argument, 0, 'l'},
		{"log-file", required_argument, 0, 'f'},
		{"show-metrics", no_argument, 0, 'm'},
		{"insecure", no_argument, 0, 'I'},
		{"set", required_argument, 0, 'e'},
		{"quiet", no_argument, 0, 'q'},
		{"version", no_argument, 0, 'V'},
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}};

	int opt;
	while ((opt = getopt_long(argc, argv, "+c:t:s:i:r:Sl:f:mhe:IqV",
				  long_opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			configuration_load_from_file(&config, optarg);
			break;
		case 't':
			config.tools_enabled = configuration_parse_tools_mask(optarg);
			break;
		case 's':
			free(config.sandbox_root);
			config.sandbox_root = strdup(optarg);
			break;
		case 'i': {
			char *end;
			long val = strtol(optarg, &end, 10);
			if (end == optarg || *end != '\0')
				val = 16;
			config.max_iterations = (int)val;
			break;
		}
		case 'r': {
			char *end;
			long val = strtol(optarg, &end, 10);
			if (end == optarg || *end != '\0')
				val = 2;
			config.max_retries = (int)val;
			break;
		}
		case 'S':
			config.stream_responses = true;
			break;
		case 'l':
			config.log_level = log_level_from_string(optarg,
								 LOG_LEVEL_INFO);
			break;
		case 'f':
			free(config.log_file);
			config.log_file = strdup(optarg);
			break;
		case 'm':
			config.show_metrics = true;
			break;
		case 'I':
			config.verify_ssl = false;
			break;
		case 'e':
			/* --set KEY=VALUE */
			{
				char *eq = strchr(optarg, '=');
				if (eq) {
					*eq = '\0';
					configuration_set_key(&config, optarg,
							      eq + 1);
				} else {
					fprintf(stderr,
						"  Invalid --set format: %s "
						"(expected KEY=VALUE)\n",
						optarg);
				}
			}
			break;
		case 'q':
			g_quiet = true;
			break;
		case 'V':
			print_version();
			configuration_destroy(&config);
			return 0;
		case 'h':
			print_usage();
			configuration_destroy(&config);
			return 0;
		default:
			print_usage();
			configuration_destroy(&config);
			return 1;
		}
	}

	/* Set up the logger */
	logger_initialize((enum log_level)config.log_level, config.log_file);

	/* Load system prompt from file if specified */
	if (config.system_prompt_file && config.system_prompt_file[0]) {
		size_t prompt_len;
		char *buf = file_read_all(config.system_prompt_file, 65535, &prompt_len);
		if (buf) {
			free(config.system_prompt);
			config.system_prompt = buf;
			log_info("System prompt loaded from %s",
				 config.system_prompt_file);
		} else {
			log_warning("Cannot load system prompt file: %s",
				    config.system_prompt_file);
		}
	}

	log_info("Boris v%s starting chat", BORIS_VERSION);

	if (!g_quiet && isatty(STDOUT_FILENO)) {
		clear_screen();
		print_banner();
	}

	/* Create the conversation */
	conv = conversation_create();
	if (!conv) {
		fprintf(stderr, "  Could not allocate memory for conversation.\n");
		configuration_destroy(&config);
		return 1;
	}

	/* Add the system prompt */
	system_prompt = config.system_prompt;
	if (!system_prompt) {
		system_prompt = "You are Boris, a helpful AI companion. "
				"You are patient, curious, and enjoy learning "
				"alongside your human.";
	}
	conversation_add_system(conv, system_prompt);

	/* Check if the model is configured */
	if (!config.model_endpoint) {
		printf("  Boris needs a model endpoint to think.\n");
		printf("\n");
		printf("  If you haven't set up yet, run: boris init\n");
		printf("\n");
		printf("  Or set the environment variable:\n");
		printf("    export BORIS_MODEL_ENDPOINT=http://localhost:8080/v1/chat/completions\n");
		printf("\n");
		conversation_destroy(conv);
		configuration_destroy(&config);
		logger_close();
		return 1;
	}

	/* Initialise HTTP client */
	http_global_init();

	/* Register tools */
	tools_register_builtins();

	/* Reset metrics for this session */
	metrics_reset();

	/* Run the interactive chat loop */
	repl_run(conv, &config);

	/* Print metrics if requested */
	if (config.show_metrics)
		metrics_print_summary();

	/* Clean up */
	http_global_cleanup();
	conversation_destroy(conv);
	configuration_destroy(&config);
	logger_close();

	return 0;
}

static void print_enabled_tools(unsigned int mask)
{
	char buf[128];
	tools_mask_to_string(mask, buf, sizeof(buf));
	printf("%s\n", buf);
}

static int cmd_status(int argc, char *argv[])
{
	struct agent_configuration config;

	configuration_load_standard(&config);

	/* Optional --config FILE to check a specific config file */
	static struct option status_opts[] = {
		{"config", required_argument, 0, 'c'},
		{"quiet", no_argument, 0, 'q'},
		{0, 0, 0, 0}};
	int opt;
	while ((opt = getopt_long(argc, argv, "c:q", status_opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			configuration_load_from_file(&config, optarg);
			break;
		case 'q':
			g_quiet = true;
			break;
		default:
			break;
		}
	}

	printf("  Boris v%s\n\n", BORIS_VERSION);
	printf("  Current Configuration:\n");
	printf("  ======================\n");
	printf("\n");
	printf("  Model endpoint  : %s\n",
	       config.model_endpoint ? config.model_endpoint : "(not set)");
	printf("  Model name      : %s\n",
	       config.model_name ? config.model_name : "(not set)");
	printf("  API key         : %s\n",
	       config.api_key ? "(set, hidden)" : "(not set)");
	printf("  Temperature     : %.1f\n", config.temperature);
	printf("  Max tokens      : %d\n", config.max_tokens);
	printf("  Context window  : %d\n", config.context_window);
	printf("  Max iterations  : %d\n", config.max_iterations);
	printf("  Max retries     : %d\n", config.max_retries);
	printf("  Timeout         : %ds\n", config.request_timeout_seconds);
	printf("  Stream          : %s\n", config.stream_responses ? "yes" : "no");
	printf("\n");
	printf("  Sandbox root    : %s\n",
	       config.sandbox_root ? config.sandbox_root : "(not set)");
	printf("  Tools enabled   : ");
	print_enabled_tools(config.tools_enabled);
	printf("  Memory persist  : %s\n", config.memory_persist ? "yes" : "no");
	printf("\n");
	printf("  Log level       : %d\n", config.log_level);
	printf("  Log file        : %s\n",
	       config.log_file ? config.log_file : "(stderr)");
	printf("  Show metrics    : %s\n", config.show_metrics ? "yes" : "no");

	if (config.system_prompt) {
		printf("\n  System prompt   : %.60s", config.system_prompt);
		if (strlen(config.system_prompt) > 60)
			printf("...");
		printf("\n");
	}

	printf("\n");

	configuration_destroy(&config);
	return 0;
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		print_usage();
		return 0;
	}

	/* Pre-scan for --quiet / -q so it works before subcommand dispatch */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0)
			g_quiet = true;
	}

	if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
		print_version();
		return 0;
	}

	if (strcmp(argv[1], "init") == 0) {
		clear_screen();
		print_banner();
		return init_run();
	}

	if (strcmp(argv[1], "chat") == 0) {
		return cmd_chat(argc - 1, argv + 1);
	}

	if (strcmp(argv[1], "status") == 0) {
		return cmd_status(argc - 1, argv + 1);
	}

	if (strcmp(argv[1], "help") == 0 ||
	    strcmp(argv[1], "--help") == 0 ||
	    strcmp(argv[1], "-h") == 0) {
		print_usage();
		return 0;
	}

	/* Unknown command */
	fprintf(stderr, "  Unknown command: %s\n\n", argv[1]);
	print_usage();
	return 1;
}
