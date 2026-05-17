/*
 * http_client.h - libcurl wrapper for HTTP requests.
 *
 * Handles synchronous POST requests and SSE streaming responses.
 * Transport errors are reported via http_response.error; HTTP-level
 * errors (non-2xx) set status_code but leave error NULL so callers
 * can extract a message from the body.
 */

#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Result of an HTTP request. Caller must call http_response_free(). */
struct http_response {
	char *body;	    /* The response body (heap-allocated, null-terminated) */
	size_t body_length; /* Length of the body in bytes */
	long status_code;   /* HTTP status code (200, 404, 500, etc.) */
	char *error;	    /* Error message if something went wrong (heap-allocated) */
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

/* Free body and error strings inside the response. */
void http_response_free(struct http_response *response);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CLIENT_H */
