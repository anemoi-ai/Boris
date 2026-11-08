/*
 * http_client.h - libcurl wrapper for HTTP requests.
 *
 * Handles synchronous POST requests (the model API) and GET requests (the
 * fetch tool), including SSE streaming responses.
 * Transport errors are reported via http_response.error; HTTP-level
 * errors (non-2xx) set status_code but leave error NULL so callers
 * can extract a message from the body.
 */

#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>
#include <stdbool.h>
#include <signal.h>

struct sockaddr;

/* Responses larger than this are aborted with an error. */
#define HTTP_MAX_RESPONSE_BYTES (32 * 1024 * 1024)

/* Result of an HTTP request. Caller must call http_response_free(). */
struct http_response {
	char *body;	    /* The response body (heap-allocated, null-terminated) */
	size_t body_length; /* Length of the body in bytes */
	long status_code;   /* HTTP status code (200, 404, 500, etc.) */
	char *error;	    /* Error message if something went wrong (heap-allocated) */

	/* Set by http_get() only (NULL / false otherwise). */
	char *content_type;  /* Content-Type header, heap-allocated */
	char *effective_url; /* final URL after redirects, heap-allocated */
	bool truncated;	     /* body cut at http_get_options.max_bytes */
};

/* Options for http_get(). Zero values select the defaults noted. */
struct http_get_options {
	int timeout_secs;	/* default 30 */
	bool verify_ssl;
	size_t max_bytes;	/* keep at most this much body; default 4 MB */
	bool allow_private;	/* permit loopback, private and link-local hosts */
	const char *user_agent; /* default "Boris" */
};

/* -------------------------------------------------------------------------
 * Global initialisation / cleanup
 * ---------------------------------------------------------------------- */

/* Call once at process startup. Returns 0 on success, -1 on failure. */
int http_global_init(void);

/* Call once at process exit. */
void http_global_cleanup(void);

/* -------------------------------------------------------------------------
 * Synchronous requests
 * ---------------------------------------------------------------------- */

/* Send a synchronous POST request. api_key may be NULL. */
struct http_response http_post(const char *url,
			       const char *body,
			       const char *content_type,
			       const char *api_key,
			       int timeout_secs,
			       bool verify_ssl);

/*
 * Called with each piece of a response body as it arrives. The bytes are
 * not NUL-terminated and pieces can split anywhere, even inside a UTF-8
 * character or an SSE line.
 */
typedef void (*http_chunk_fn)(const char *data, size_t len, void *ctx);

/*
 * http_post(), but on_chunk sees the body as it arrives (true streaming).
 * The whole body is still buffered and returned as usual, so callers parse
 * the complete response exactly as they would from http_post().
 */
struct http_response http_post_stream(const char *url,
				      const char *body,
				      const char *content_type,
				      const char *api_key,
				      int timeout_secs,
				      bool verify_ssl,
				      http_chunk_fn on_chunk,
				      void *chunk_ctx);

/*
 * Send a GET request for the fetch tool.
 *
 * Only http and https are allowed, including across redirects (at most 5).
 * Unless opts->allow_private is set, connections to loopback, private,
 * link-local and other non-public addresses are refused at connect time,
 * after DNS resolution, so redirects and DNS tricks cannot reach them;
 * proxies from the environment are not used, so they cannot bypass that
 * check. A body longer than max_bytes is cut and sets truncated.
 */
struct http_response http_get(const char *url, const struct http_get_options *opts);

/* True if addr (AF_INET or AF_INET6) is not a public internet address. */
bool http_address_is_private(const struct sockaddr *addr);

/*
 * Register a flag that aborts in-flight requests when it becomes non-zero
 * (typically set from a SIGINT handler). Pass NULL to clear it. An aborted
 * request returns with error set.
 */
void http_set_cancel_flag(const volatile sig_atomic_t *flag);

/* Free body and error strings inside the response. */
void http_response_free(struct http_response *response);


#endif /* HTTP_CLIENT_H */
