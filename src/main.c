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
#include "fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

static bool g_quiet = false;

#define BORIS_VERSION "1.0.0"

/* Upper bound on configuration-changing flags in one invocation. */
#define MAX_CLI_SETTINGS 64

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
	printf("                        (read,write,list_dir,memory,run,all,none)\n");
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
	printf("  Settings are layered: defaults, ~/.boris/config.ini, --config FILE,\n");
	printf("  BORIS_* environment variables, then command-line flags.\n");
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

/*
 * Settings collected from the command line, applied in order after the
 * file and environment layers so that flags always win.
 */
struct cli_settings {
	const char *config_file; /* --config, applied before the environment */
	const char *keys[MAX_CLI_SETTINGS];
	const char *values[MAX_CLI_SETTINGS];
	int count;
};

static void cli_add(struct cli_settings *cli, const char *key, const char *value)
{
	if (cli->count >= MAX_CLI_SETTINGS) {
		fprintf(stderr, "  Too many options; ignoring %s\n", key);
		return;
	}
	cli->keys[cli->count] = key;
	cli->values[cli->count] = value;
	cli->count++;
}

/* Build the configuration from every layer, lowest priority first. */
static void configuration_load_layers(struct agent_configuration *config,
				      const struct cli_settings *cli)
{
	configuration_set_defaults(config);

	char *path = fs_boris_path("config.ini");
	if (path) {
		configuration_load_from_file(config, path);
		free(path);
	}
	if (cli && cli->config_file)
		configuration_load_from_file(config, cli->config_file);

	configuration_apply_environment(config);

	for (int i = 0; cli && i < cli->count; i++)
		if (configuration_set_key(config, cli->keys[i], cli->values[i]) != BORIS_OK)
			fprintf(stderr, "  Invalid option: %s = %s\n",
				cli->keys[i], cli->values[i]);
}

/* Replace the inline system prompt with the contents of system_prompt_file. */
static void load_system_prompt_file(struct agent_configuration *config)
{
	if (!config->system_prompt_file || !config->system_prompt_file[0])
		return;

	size_t prompt_len;
	char *buf = file_read_all(config->system_prompt_file, 65535, &prompt_len);
	if (buf && buf[0]) {
		free(config->system_prompt);
		config->system_prompt = buf;
		log_info("System prompt loaded from %s", config->system_prompt_file);
	} else {
		free(buf);
		log_warning("Cannot load system prompt file (missing or empty): %s",
			    config->system_prompt_file);
	}
}

static int cmd_chat(int argc, char *argv[])
{
	struct agent_configuration config;
	struct cli_settings cli;
	memset(&cli, 0, sizeof(cli));

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
			cli.config_file = optarg;
			break;
		case 't':
			cli_add(&cli, "tools.enabled", optarg);
			break;
		case 's':
			cli_add(&cli, "tools.sandbox_root", optarg);
			break;
		case 'i':
			cli_add(&cli, "agent.max_iterations", optarg);
			break;
		case 'r':
			cli_add(&cli, "agent.max_retries", optarg);
			break;
		case 'S':
			cli_add(&cli, "model.stream", "true");
			break;
		case 'l':
			cli_add(&cli, "logging.level", optarg);
			break;
		case 'f':
			cli_add(&cli, "logging.file", optarg);
			break;
		case 'm':
			cli_add(&cli, "output.show_metrics", "true");
			break;
		case 'I':
			cli_add(&cli, "model.verify_ssl", "false");
			break;
		case 'e': { /* --set KEY=VALUE */
			char *eq = strchr(optarg, '=');
			if (eq) {
				*eq = '\0';
				cli_add(&cli, optarg, eq + 1);
			} else {
				fprintf(stderr,
					"  Invalid --set format: %s "
					"(expected KEY=VALUE)\n",
					optarg);
			}
			break;
		}
		case 'q':
			g_quiet = true;
			break;
		case 'V':
			print_version();
			return 0;
		case 'h':
			print_usage();
			return 0;
		default:
			print_usage();
			return 1;
		}
	}

	/* --quiet also hides the per-tool activity lines (applied last, so it
	 * wins over the config file) */
	if (g_quiet)
		cli_add(&cli, "output.show_tool_calls", "false");

	/* Log warnings from configuration loading to stderr, then switch to
	 * the configured level and destination. */
	logger_initialize(LOG_LEVEL_WARNING, NULL);
	configuration_load_layers(&config, &cli);
	logger_initialize((enum log_level)config.log_level, config.log_file);

	load_system_prompt_file(&config);

	log_info("Boris v%s starting chat", BORIS_VERSION);

	/* Check if the model is configured */
	if (!config.model_endpoint) {
		printf("  Boris needs a model endpoint to think.\n");
		printf("\n");
		printf("  If you haven't set up yet, run: boris init\n");
		printf("\n");
		printf("  Or set the environment variable:\n");
		printf("    export BORIS_MODEL_ENDPOINT=http://localhost:8080/v1/chat/completions\n");
		printf("\n");
		configuration_destroy(&config);
		logger_close();
		return 1;
	}

	/* Create the conversation, seeded with the system prompt */
	struct conversation_history *conv = conversation_create();
	if (!conv ||
	    conversation_add_system(conv, config.system_prompt
						  ? config.system_prompt
						  : BORIS_DEFAULT_SYSTEM_PROMPT) != BORIS_OK) {
		fprintf(stderr, "  Could not allocate memory for conversation.\n");
		conversation_destroy(conv);
		configuration_destroy(&config);
		logger_close();
		return 1;
	}

	if (!g_quiet && isatty(STDOUT_FILENO)) {
		term_erase_screen();
		print_banner();
	}

	/* A script that exits without reading its stdin, or a server that
	 * drops the connection, must not kill the process with SIGPIPE. */
	signal(SIGPIPE, SIG_IGN);

	if (http_global_init() != 0)
		fprintf(stderr, "  Warning: HTTP initialisation failed.\n");

	tools_register_builtins();
	metrics_reset();

	/* Run the interactive chat loop */
	repl_run(conv, &config);

	if (config.show_metrics)
		metrics_print_summary();

	http_global_cleanup();
	conversation_destroy(conv);
	configuration_destroy(&config);
	logger_close();

	return 0;
}

static int cmd_status(int argc, char *argv[])
{
	struct agent_configuration config;
	struct cli_settings cli;
	memset(&cli, 0, sizeof(cli));

	/* Optional --config FILE to check a specific config file */
	static struct option status_opts[] = {
		{"config", required_argument, 0, 'c'},
		{"quiet", no_argument, 0, 'q'},
		{0, 0, 0, 0}};
	int opt;
	while ((opt = getopt_long(argc, argv, "c:q", status_opts, NULL)) != -1) {
		switch (opt) {
		case 'c':
			cli.config_file = optarg;
			break;
		case 'q':
			g_quiet = true;
			break;
		default:
			break;
		}
	}

	logger_initialize(LOG_LEVEL_WARNING, NULL);
	configuration_load_layers(&config, &cli);

	char tools_buf[128];
	tools_mask_to_string(config.tools_enabled, tools_buf, sizeof(tools_buf));

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
	printf("  Tools enabled   : %s\n", tools_buf);
	printf("  Confirm writes  : %s\n", config.confirm_writes ? "yes" : "no");
	printf("  Confirm run     : %s\n", config.confirm_bash ? "yes" : "no");
	printf("  Memory persist  : %s\n", config.memory_persist ? "yes" : "no");
	printf("\n");
	printf("  Log level       : %d\n", config.log_level);
	printf("  Log file        : %s\n",
	       config.log_file ? config.log_file : "(stderr)");
	printf("  Show metrics    : %s\n", config.show_metrics ? "yes" : "no");

	if (config.system_prompt_file)
		printf("\n  System prompt   : (from %s)\n", config.system_prompt_file);
	else if (config.system_prompt)
		printf("\n  System prompt   : %.60s%s\n", config.system_prompt,
		       strlen(config.system_prompt) > 60 ? "..." : "");

	printf("\n");

	configuration_destroy(&config);
	logger_close();
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
		if (!g_quiet && isatty(STDOUT_FILENO)) {
			term_erase_screen();
			print_banner();
		}
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
