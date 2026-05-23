# WALKTHROUGH: the agent loop

This is a long-form walk through `src/agent.c` — the central file in Boris, the one that turns a chat-completions API into something that can actually *do* things. It's the file most people will want to read first, and it's the file most worth reading carefully. Everything else in the codebase exists to serve what happens here.

If you've only ever seen agent code that looks like `agent.run(prompt)`, this is for you. There is no `agent.run`. There is a `while` loop, a function-local state, and about five hundred lines of careful bookkeeping between what the model says and what it sees next. The bookkeeping is the part the framework documentation never shows you, and it's the whole reason this file exists.

I'll show you the shape, then I'll walk through the parts the shape glosses over, then I'll tell you what I got wrong the first three times.

---

## The shape, first

You've probably seen this already in the README:

```c
while (iterations < cfg->max_iterations) {
    llm_complete_with_retry(conv, tools, num_tools, cfg);

    if (finish_reason == "stop")
        return FINISHED_NATURALLY;

    if (finish_reason == "tool_calls")
        dispatch_tool_calls();      /* append results, loop */

    if (finish_reason == "length")
        truncate_and_continue();    /* context overflow */
}
return FINISHED_MAX_ITERATIONS;
```

That's the agent. Every framework you have ever used — LangChain, CrewAI, AutoGen, the OpenAI Assistants API, your company's home-grown thing — is a variation of this. The variations are not subtle, but they are not what they appear to be either. They are mostly bookkeeping. The loop is the same.

If you stop reading here, you've already got the core idea. Good news: the core idea is not the hard part. Bad news: the hard part is everything the five-line version doesn't say.

Here is an incomplete list of things you have to decide that the skeleton above doesn't tell you:

- The conversation lives where? Owned by whom? Freed by whom?
- What happens when the network call fails — not "the model returned an error", but when the TCP connection drops mid-stream?
- A tool returns. Where does its output go in the conversation, in what format, in what order if there are several?
- The model decides to call the same tool with the same arguments three turns in a row. Do you let it? Do you stop it? How do you tell?
- The response gets cut off because it hit `max_tokens`. Do you ask it to continue? How? Forever?
- You're about to overflow the context window. Which messages do you drop? In what order? Do you tell the model you dropped them?
- The user hits Ctrl+C in the middle of a tool call. What state is everything in when the signal handler returns?
- The model emits a tool call as a JSON code block in its reply, because it's a local model that doesn't speak the native `tool_calls` protocol. Do you parse it? Do you trust it?

None of these are exotic. They all happen on the first afternoon you try to run an agent against a real workload. Most tutorials handle approximately none of them. The job of `agent.c` is to handle all of them, in C, without losing your mind.

So let's go through it.

---

## All the state lives on the stack

Open `agent.c` and look for `run_loop`. Inside the first thirty lines or so you will find this:

```c
int iterations = 0;
int total_prompt_tok = 0;
int total_completion_tok = 0;
int continuation_count = 0;

int consecutive_tool_errors = 0;
int consecutive_toolcall_turns = 0;
int last_error_reminder_iter = -REMINDER_COOLDOWN_ITERS;
int last_truncation_reminder_iter = -REMINDER_COOLDOWN_ITERS;
int last_textfallback_reminder_iter = -REMINDER_COOLDOWN_ITERS;
int last_repeat_reminder_iter = -REMINDER_COOLDOWN_ITERS;
int last_drift_reminder_iter = -REMINDER_COOLDOWN_ITERS;
uint64_t last_tool_fingerprint = 0;
```

These are all the variables that change *across* iterations of the loop. There are about a dozen of them. Note where they live: locals in `run_loop`. There is no agent object. There is no global state. (There's one global, a `sig_atomic_t` flag for SIGINT, and I'll get to that.) The bookkeeping has the same lifetime as the call to `run_loop`. When the function returns, all of this evaporates and the only thing left is the conversation, which the caller owns.

This is on purpose, and it matters more than it looks.

One of the things C teaches you very firmly, if you let it, is that **state with unclear lifetime is the source of most production bugs.** Frameworks like to hide state inside objects whose lifetimes you don't really think about. They get away with it because the GC sweeps up afterwards, but they accumulate subtle bugs — a counter that was supposed to reset between runs, a cache that lives one request too long, a callback registered against the wrong instance.

In C you have no choice but to think about lifetime. Once you've spent a few months thinking about lifetime, you start writing better code in every language. The discipline transfers. This is one of the harder-to-articulate reasons for writing an agent in C as a learning exercise: it forces you to confront things you can ignore everywhere else.

Each of those counters also has a story. Let me give you the short version.

`iterations` is the loop counter, bounded by `cfg->max_iterations`. The model gets a finite number of turns to do its job, and then the loop terminates with `FINISHED_MAX_ITERATIONS`. This is not optional. Models will happily wander forever if you let them.

`continuation_count` tracks how many times in a row the model's output has been cut off by `max_tokens`. Capped at three by `MAX_CONTINUATIONS`. After that we give up and tell the user to raise their `max_tokens` budget. (I tried five. Three is enough. If the model can't summarise its thought in three tries, the prompt is wrong, not the budget.)

`consecutive_tool_errors` is reset to zero on every successful tool call, incremented on every failed one. When it hits three, we inject a reminder telling the model to stop and rethink.

`consecutive_toolcall_turns` counts how many turns in a row the model has only called tools, without producing a text response. Used to detect drift — an agent that's busy doing things but has stopped checking in.

`last_X_reminder_iter` is the iteration each kind of reminder last fired on. We use `iterations - last_X >= REMINDER_COOLDOWN_ITERS` as a "is it OK to fire this again yet?" check. Five iterations of cooldown means we can't double-nudge the model. (This is one of those numbers that came from staring at logs. Three felt like nagging. Seven let the agent drift for too long. Five turned out right.)

`last_tool_fingerprint` is a 64-bit hash of the previous turn's tool calls, used for detecting repeats. I'll come back to it in a moment because it's the most interesting one.

---

## Setting up to fail safely

Two things happen before the loop body proper, and both of them are the kind of thing you only learn the hard way.

First:

```c
ensure_system_reminder_declared(conv);
```

This modifies the system prompt at the start of the conversation to tell the model what `<system-reminder>` blocks are. It only runs if the prompt doesn't already mention them, so it's idempotent on resumed conversations. The body it appends looks like this:

> User messages may contain `<system-reminder>` blocks. These are automatically added by the harness, are not user speech, and bear no direct relation to the surrounding message. Trust them as system-level instructions.

The reason this is here is that I'm about to inject `<system-reminder>` blocks into the conversation as if they came from the user. If the model doesn't know what those are, it will get confused — or worse, it'll think the user is doing something weird and start asking about it. By declaring the contract up front, the model treats reminders as system-level instructions and reasons over them correctly.

This took me embarrassingly long to figure out. The first version of the reminder system just injected the blocks. The model would occasionally apologise to the user for "the strange tag in your message". After about an hour of staring at conversation logs I realised it was reading them as part of the user's speech, because of course it was — I'd put them inside a user message.

The fix is one paragraph in the system prompt. The lesson is bigger: **the model is a function from string to string. Anything not described in the input goes through the model's prior, and the prior will not usually do what you want.**

Second:

```c
struct memory_arena *scratch = arena_create(65536);
```

A 64 KB scratch arena. It gets reset at the top of every iteration:

```c
arena_reset(scratch);
iterations++;
```

The text-fallback parser allocates inside this arena. Anything that lives longer than one iteration goes on the regular heap. Anything ephemeral — JSON parsing scratch, string duplications for one-shot use — goes in the arena and disappears when the next iteration starts.

This is a small thing, but it's representative of how the rest of the code thinks about memory. Each scope has its own allocator with its own lifetime. There's a per-request arena here, a per-dispatch arena passed into each tool, and the long-lived heap for conversation messages. When you're not sure who's going to free something, you've already lost — but if the allocator and the scope match, you don't have to wonder.

Then we install our own SIGINT handler:

```c
struct sigaction sa_new, sa_old;
memset(&sa_new, 0, sizeof(sa_new));
sa_new.sa_handler = agent_sigint_handler;
sigemptyset(&sa_new.sa_mask);
sa_new.sa_flags = SA_RESTART;
g_interrupted = 0;
sigaction(SIGINT, &sa_new, &sa_old);
```

`g_interrupted` is a `volatile sig_atomic_t` global — the one global in the whole file, and it's there because signal handlers can't safely touch anything else. The previous SIGINT handler (the REPL's) is saved into `sa_old` and restored at cleanup. `SA_RESTART` means a Ctrl+C during a blocking syscall like `read()` or `write()` will return `EINTR` rather than aborting it; libcurl handles this gracefully.

The handler itself is two lines:

```c
static void agent_sigint_handler(int sig)
{
    (void)sig;
    g_interrupted = 1;
}
```

The flag gets checked at iteration boundaries, *and* immediately after `llm_complete_with_retry` returns. Two checkpoints, both important. The first catches a Ctrl+C between iterations. The second catches one that came in while we were blocked on the network. Get this wrong and you'll have a confusing "the program ignored my Ctrl+C" bug that's about 30 seconds long every time.

---

## The LLM call (and what "with retry" actually means)

The first thing in the loop body proper is this call:

```c
resp = llm_complete_with_retry(
    conv, tools, num_tools, cfg, call_err, sizeof(call_err));
```

It looks like one line. It is not one line. Look at the function:

```c
static struct llm_response *llm_complete_with_retry(
    const struct conversation_history *conv,
    const struct tool_definition *tools,
    size_t num_tools,
    const struct agent_configuration *cfg,
    char *err_out, size_t err_size)
{
    struct llm_response *resp = llm_complete(conv, tools, num_tools, cfg);
    if (!resp) { ... }
    if (!resp->error)
        return resp;
    ...
    int transient = is_transient_error(resp);
    llm_response_free(resp);

    if (!transient || cfg->max_retries <= 0)
        return NULL;

    for (int i = 0; i < cfg->max_retries; i++) {
        int backoff_ms = cfg->retry_backoff_ms * (1 << i);
        ...
        sleep_ms(backoff_ms);
        ...
```

Two things to notice. First, `is_transient_error` decides what's worth retrying. A 5xx, a timeout, a dropped connection: transient, retry. A 400, an auth failure, an OOM building the request: permanent, don't retry into a wall. The exact check is in the function and it's deliberately conservative — when in doubt, treat as permanent. A misbehaving retry loop hammers your provider; a missed retry annoys one user.

Second, the backoff is exponential: `cfg->retry_backoff_ms * (1 << i)`. Default `retry_backoff_ms` is small, default `max_retries` is two, so the typical pattern is something like 500 ms, 1 s, give up. You can tune both. The `(1 << i)` is just `pow(2, i)` in integer form — `1`, `2`, `4`, `8`. (Real production systems add jitter so that a thousand clients all hitting the same rate limit don't all retry at the same instant and re-hit it together. Boris doesn't, because Boris is usually one client. If you're embedding it in a multi-tenant server, add jitter.)

By the time we get back to the loop body, one of three things has happened: we have a parsed response in hand, we have a permanent error and `resp` is `NULL`, or we caught a SIGINT during a retry sleep and `g_interrupted` is set. The loop body checks for the SIGINT first, then for `NULL`, then proceeds with `resp`.

This is one of the most important properties of a well-written C program. **Layers below you should make it safe to be naive at your level.** If `llm_complete_with_retry` left the conversation in a half-mutated state on failure, every caller would have to know about that and clean up. It doesn't — the conversation isn't touched until we know we have a clean response. So the loop body can be short, because it doesn't have to defend against problems that were already handled below it.

---

## Three cases in order, and one trick

OK, we have `resp`. Now we dispatch on `finish_reason`. There are three cases the loop cares about, and the order of the checks matters.

**Case 1: `stop` with no tool calls.** The model decided it's done. Append its message to the conversation, build a `FINISHED_NATURALLY` result, return.

Except — and here's the first thing that surprised me — we don't return immediately. If `cfg->text_tool_fallback` is on and the response has content, we first try to parse the content for tool calls embedded in the text:

```c
if (strcmp(resp->finish_reason, LLM_FINISH_STOP) == 0 &&
    resp->tool_calls.count == 0) {
    if (cfg->text_tool_fallback && resp->content) {
        ...
        int nc = parse_text_tool_calls(resp->content, &text_calls, scratch);
        if (nc > 0) {
            ...synthesize a fake tool_calls turn and continue the loop
        }
    }
    append_assistant_turn(conv, resp);
    result = make_result(FINISHED_NATURALLY, ...);
    goto cleanup;
}
```

The reason: many local models don't support the OpenAI `tool_calls` channel natively. They emit tool calls as JSON inside markdown code fences, or inside `<tool_call>` XML tags. They then finish with `finish_reason == "stop"` because as far as they're concerned, they responded.

If we accept that at face value, the agent ends prematurely and the user sees a JSON blob in their terminal. If we parse it, the agent works against models that the OpenAI SDK won't even talk to.

`parse_text_tool_calls` does two passes. First it looks for `<tool_call>...</tool_call>` XML tags. If that finds nothing, it looks for triple-backtick code fences whose content starts with `{`. The `{` check is what stops us from parsing every shell snippet the model writes as a tool call. (Ask me how I know.)

If we find any, we synthesise a fake `llm_response` with the parsed calls and a `finish_reason` of `"tool_calls"`, append that to the conversation, and run the same dispatch path as Case 2 below. We also inject a reminder telling the model to prefer the native channel next time — which sometimes actually works, especially with the better-behaved local models.

This whole branch is what makes Boris usable with llama.cpp and Ollama out of the box, instead of being an OpenAI-only toy. It's about thirty lines of code. It is, hands-down, the highest-leverage thirty lines in the file.

**Case 2: `tool_calls`.** The model wants to call one or more tools. Append the assistant turn (with its tool-call requests) to the conversation, then for each tool call, dispatch it and append its result. The results are linked back to their calls by `tool_call_id` — that's the contract the OpenAI wire format requires. The result message has its own role (`MESSAGE_ROLE_TOOL` in `boris_types.h`), distinct from `MESSAGE_ROLE_USER`. Mix these up and the model will get confused about who said what.

But before we even get there, we compute the *turn fingerprint*:

```c
uint64_t this_turn_fp = 0;
for (size_t ti = 0; ti < resp->tool_calls.count; ti++)
    this_turn_fp ^= tool_call_fingerprint(
        &resp->tool_calls.calls[ti]);

bool repeated_calls = (last_tool_fingerprint != 0 &&
                       this_turn_fp == last_tool_fingerprint);
last_tool_fingerprint = this_turn_fp;
```

`tool_call_fingerprint` hashes the tool name and arguments together using FNV-1a:

```c
static uint64_t tool_call_fingerprint(const struct tool_call *tc)
{
    uint64_t h = fnv1a_64(tc->name);
    h ^= fnv1a_64(tc->argument_json);
    h *= 0x100000001b3ULL;
    return h;
}
```

And the per-turn fingerprint is the XOR of all the per-call fingerprints. XOR is commutative, so the order doesn't matter — calling `[read("a.txt"), write("b.txt")]` and `[write("b.txt"), read("a.txt")]` produce the same turn fingerprint. That's the right semantic. We want to detect "the model did the same thing twice", and "same thing" doesn't depend on the order of independent tool calls within a turn.

FNV-1a is the right choice for this because it's cheap, has decent avalanche, and produces a 64-bit hash with a tiny code footprint. We don't need cryptographic strength — a collision here means we suppress a single duplicate-call warning, which is harmless. The alternative would be a real string-equality check across every prior turn's tool calls, which is more code, more allocations, and not noticeably more correct.

If `this_turn_fp == last_tool_fingerprint`, the model just made the same tool calls as last turn. After we dispatch and append the results, we'll inject a reminder. We'll get there.

The actual dispatch is a `for` loop over the tool calls:

```c
for (size_t ti = 0; ti < resp->tool_calls.count; ti++) {
    struct tool_call *tc = &resp->tool_calls.calls[ti];
    ...
    struct tool_result result_tc = tools_dispatch(tc, cfg);
    metrics_record_tool_call();

    consecutive_tool_errors = result_tc.is_error
                              ? consecutive_tool_errors + 1
                              : 0;
    ...
    append_tool_result(conv, &result_tc);
    tool_result_free(&result_tc);
}
```

A successful tool call resets `consecutive_tool_errors` to zero. A failed one increments it. The counter is global to the *run*, not to a single turn — three errors across three turns trigger the reminder just as readily as three in one turn. That's deliberate; if the model is consistently misusing one tool, we want to nudge it regardless of pacing.

After the loop, we increment `consecutive_toolcall_turns`, and then we decide whether to inject a reminder. Three independent checks:

```c
if (consecutive_tool_errors >= 3 && /* cooldown OK */) {
    inject_reminder(conv, "3 consecutive tool calls have failed...");
    last_error_reminder_iter = iterations;
    consecutive_tool_errors = 0;
} else if (repeated_calls && /* cooldown OK */) {
    inject_reminder(conv, "You just issued the same tool call(s)...");
    last_repeat_reminder_iter = iterations;
} else if (consecutive_toolcall_turns >= DRIFT_STUCK_THRESHOLD && /* cooldown OK */) {
    inject_reminder(conv, "You have been executing tools for several turns...");
    last_drift_reminder_iter = iterations;
    consecutive_toolcall_turns = 0;
}
```

Three reminders, three different reset behaviours. The error reminder resets the error counter — we've nudged, we'll wait to see if it improves. The repeat reminder doesn't reset anything; the fingerprint check naturally suppresses itself once the model changes behaviour. The drift reminder resets the drift counter — same logic as the error reminder. These three reset patterns came out of watching the loop run, not from designing them up front. The naive "reset everything" version was annoying in different ways for different kinds of agent stuckness.

They're also `else if`, not three independent `if`s. We inject at most one reminder per turn. Stacking three reminders on top of each other into a single user message confuses the model and makes it harder to debug what fired.

**Case 3: `length`.** The model's response was cut off. This is the case that taught me the most.

The OpenAI API returns `finish_reason == "length"` in two very different situations, and you have to handle them differently:

1. The completion hit the configured `max_tokens` cap. The model was mid-thought, but ran out of output budget.
2. The completion ran out of *context window* — the prompt plus the response together exceeded the model's window.

From the outside they look identical. The only way to tell them apart is to estimate the current conversation's token count and compare against the configured context window:

```c
size_t estimated = conversation_estimate_tokens(conv);
size_t target = (size_t)(cfg->context_window * 3 / 4);
if (estimated > target && conv->count > 3) {
    /* context window full — truncate and continue */
    conversation_truncate(conv, target);
    inject_reminder(conv, "The conversation was truncated...");
    continue;
}

/* otherwise, max_tokens was hit — ask the model to continue */
if (continuation_count < MAX_CONTINUATIONS) {
    continuation_count++;
    inject_reminder(conv,
                    "Your previous response was cut off by the "
                    "max_tokens limit. Continue from where you left off...");
    continue;
}
```

Truncation hits the last three-quarters mark; continuation gets capped at three. The reminder text differs in each case because the model has to do different things to recover. After a truncation, earlier results may no longer be visible and need to be re-fetched. After a `max_tokens` cut-off, the next response should just continue where the last one stopped.

The `conv->count > 3` check is there so we don't truncate a conversation that's just the system prompt and a single user message — there's nothing useful to truncate, so it can't be a context-window problem and must be a `max_tokens` problem.

This is one of the cases the README only hints at. Getting it right took me longer than it should have, mostly because the first version assumed `finish_reason == "length"` always meant `max_tokens`. The agent would loop forever asking the model to continue a response that was being cut off by context overflow, not by `max_tokens`. Every continuation made the context bigger, which made the next continuation cut off sooner, which… you can see where this is going.

Two different problems, two different fixes. Same wire-format symptom.

There's also a fourth case the dispatch falls through to:

```c
log_warning("agent: unexpected finish_reason '%s'", resp->finish_reason);
if (resp->content && resp->content[0]) {
    append_assistant_turn(conv, resp);
    result = make_result(FINISHED_NATURALLY, ...);
    goto cleanup;
}
```

If the API returns a `finish_reason` we don't know about (`content_filter`, `function_call`, or something we've never seen), and the model produced *some* text, treat it as a natural finish. Only error out if the unknown finish reason came with no content. This is a "be liberal in what you accept" choice that pays off every time you swap LLM providers and discover that, say, the gpt-OSS family has a few finish reasons of its own.

---

## What `inject_reminder` is actually doing

I've referred to "injecting reminders" several times without saying what they look like. Here's the function:

```c
static void inject_reminder(struct conversation_history *conv, const char *body)
{
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "<system-reminder>\n%s\n</system-reminder>", body);
    conversation_add_user(conv, buf);
    log_debug("agent: injected reminder: %.80s", body);
}
```

That's it. We wrap the body in a `<system-reminder>` block and add it as a user message. The model has been told (back at the start of the loop, in `ensure_system_reminder_declared`) that these blocks are system-level instructions, not user speech.

The buffer is 1 KB on the stack. The reminder bodies are all short — twenty lines of English at most — so 1 KB is enough. Truncation would be a bug, not a feature, so I'd rather catch it in code review than handle it silently.

You'll notice some of the reminder strings contain `"\xe2\x80\x94"` — that's a UTF-8 em dash inside a C string literal. The model reads it as a real em dash. Worth it.

---

## What I got wrong

A few things, off the top of my head:

**I tried to make the loop generic.** The first version had a callback table for `finish_reason` handlers, so the dispatch logic could be extended without touching the loop. It was beautiful. It was also impossible to read, because the actual control flow was hidden in a struct of function pointers somewhere else. The current version is one big function with three branches and a fallthrough, and it fits in your head. Generic was wrong. Specific was right.

**I let the model retry its own tool calls.** Early on, I figured if a tool failed, the model would notice and try something different. Sometimes it did. Sometimes it called the same tool with the same arguments six times in a row. The fingerprint check came out of staring at six identical log lines and asking myself why I was paying for them.

**I tracked everything per-turn at first.** Errors per turn. Repeats per turn. Drift per turn. None of it worked, because models are not consistent about timing. A model can fail a tool three times across three turns and not notice. The counters had to be cumulative-with-cooldown, not per-turn. This is also why each counter has its own reset semantics rather than a unified "clear all counters" call — they each capture a different invariant.

**I didn't save the previous SIGINT handler.** The first version just installed the agent's handler and never restored it. When the agent returned, the REPL lost the ability to handle Ctrl+C properly. Took me an embarrassingly long time to figure out, because it only happened *after* an agent run, not before.

**I tried clever truncation.** "Drop the oldest tool results first." "Compress earlier turns into summaries." "Keep all assistant messages, drop all tool results above a threshold." Each version sounded smart. Each version broke something. The current version truncates from the front (after the system prompt), keeping recent context, and tells the model what happened so it can re-fetch what it needs. Boring. Works. The lesson there is the lesson of the entire file: **the simplest thing that respects the invariants beats the cleverest thing that doesn't.**

---

## Where to read next

If this was interesting, here's what to read after `agent.c`, in order of how strongly I'd recommend it:

- `src/llm.c` — what `llm_complete` actually does. This is where the HTTP request gets built, the response gets parsed, and the OpenAI wire format becomes a `struct llm_response`.
- `src/conversation.c` — the conversation history data structure. How `append_assistant_turn` and `append_tool_result` actually work, why messages are individually heap-allocated, how truncation preserves the system prompt.
- `src/tools.c` and `src/tools/*.c` — the tool registry and individual tool implementations. Read `tools/write.c` for the atomic-write-via-rename pattern; read `tools/run.c` for the fork/exec/timeout dance.
- `src/sandbox.c` — why `realpath()` before every tool call, and what naive path-prefix checking gets wrong.
- `tests/mock_http.c` and `tests/test_e2e.c` — how the entire agent loop gets tested without a real LLM. The mock implements the same `http_post()` interface as the real client and is linked in place of it.

The bigger picture: every line of `agent.c` is a load-bearing line. The loop itself is five lines. Everything else is the result of running real workloads through that loop and watching what breaks. If you take one thing away from this file, take this: **the gap between "the agent loop is just five lines" and "you have a production agent" is the entire rest of `agent.c`, and the only way to learn it is to write it.**

That's the whole pitch, really. Read the file. Make notes. Then go write your own.
