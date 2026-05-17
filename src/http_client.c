/*
 * http_client.c - libcurl wrapper for HTTP POST requests.
 *
 * Response bodies are accumulated via a libcurl write callback into a
 * growing heap buffer and returned as a single allocation.
 */

#define _POSIX_C_SOURCE 200809L

#include "http_client.h"
#include "logger.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * The buffer we use to collect response data.
 *
 * As chunks arrive, we grow this buffer to fit them all.
 * The capacity is the total allocated size, length is what
 * we've actually received.
 */
struct response_buffer {
	char *data;
	size_t length;
	size_t capacity;
};

/* Global initialisation state */
static int g_http_initialised = 0;

int http_global_init(void)
{
	if (g_http_initialised)
		return 0;

	CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (rc != CURLE_OK) {
		log_error("http: curl_global_init failed: %s",
			  curl_easy_strerror(rc));
		return -1;
	}
	g_http_initialised = 1;
	return 0;
}

void http_global_cleanup(void)
{
	if (g_http_initialised) {
		curl_global_cleanup();
		g_http_initialised = 0;
	}
}

/*
 * libcurl write callback. Appends each received chunk to the response buffer,
 * doubling capacity as needed. Returns chunk_size on success; returning 0
 * signals a write error to libcurl and aborts the transfer.
 */
static size_t write_callback(void *contents, size_t size, size_t nmemb,
			     void *userdata)
{
	size_t chunk_size = size * nmemb;
	struct response_buffer *buf = (struct response_buffer *)userdata;
	size_t new_length = buf->length + chunk_size;

	/* Grow the buffer if needed */
	if (new_length >= buf->capacity) {
		size_t new_capacity = buf->capacity * 2;
		char *new_data;

		if (new_capacity < new_length + 1)
			new_capacity = new_length + 1;

		new_data = realloc(buf->data, new_capacity);
		if (!new_data)
			return 0; /* Signal error to libcurl */

		buf->data = new_data;
		buf->capacity = new_capacity;
	}

	/* Copy the chunk */
	memcpy(buf->data + buf->length, contents, chunk_size);
	buf->length = new_length;
	buf->data[buf->length] = '\0'; /* Null-terminate for safety */

	return chunk_size;
}

/*
 * Shared curl setup helper.
 *
 * Configures a curl easy handle with common options:
 * URL, timeout, write callback, follow redirects, SSL settings.
 */
static void curl_setup_common(CURL *curl, const char *url, int timeout_secs,
			      struct response_buffer *buf, bool verify_ssl)
{
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_secs);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verify_ssl ? 1L : 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verify_ssl ? 2L : 0L);
}

/*
 * Send a POST request with arbitrary content type.
 */
struct http_response http_post(const char *url,
			       const char *body,
			       const char *content_type,
			       const char *api_key,
			       int timeout_secs,
			       bool verify_ssl)
{
	CURL *curl;
	CURLcode result;
	struct http_response response;
	struct response_buffer buf;
	struct curl_slist *headers = NULL;

	/* Initialize the response to a clean state */
	memset(&response, 0, sizeof(struct http_response));

	if (!url) {
		response.error = strdup("URL is required");
		return response;
	}

	/* Ensure curl is initialised */
	if (!g_http_initialised)
		http_global_init();

	curl = curl_easy_init();
	if (!curl) {
		response.error = strdup("Failed to initialize libcurl");
		return response;
	}

	/* Set up the response buffer */
	buf.data = malloc(4096);
	buf.length = 0;
	buf.capacity = 4096;
	if (!buf.data) {
		response.error = strdup("Out of memory");
		curl_easy_cleanup(curl);
		return response;
	}
	buf.data[0] = '\0';

	/* Configure common options */
	curl_setup_common(curl, url, timeout_secs, &buf, verify_ssl);

	/* POST-specific options */
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	if (body)
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

	/* Set headers */
	char ct_header[256];
	snprintf(ct_header, sizeof(ct_header), "Content-Type: %s",
		 content_type ? content_type : "application/octet-stream");
	headers = curl_slist_append(headers, ct_header);
	headers = curl_slist_append(headers, "Accept: application/json");

	if (api_key && api_key[0]) {
		char auth_header[1024];
		snprintf(auth_header, sizeof(auth_header),
			 "Authorization: Bearer %s", api_key);
		headers = curl_slist_append(headers, auth_header);
	}

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	/* Send the request */
	result = curl_easy_perform(curl);

	if (result != CURLE_OK) {
		response.error = strdup(curl_easy_strerror(result));
		log_error("HTTP request failed: %s", response.error);
	} else {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE,
				  &response.status_code);

		response.body = buf.data;
		response.body_length = buf.length;
		buf.data = NULL;

		if (response.status_code < 200 ||
		    response.status_code >= 300)
			log_error("HTTP error %ld", response.status_code);
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (buf.data)
		free(buf.data);

	return response;
}

/*
 * Free an HTTP response and all its allocated memory.
 */
void http_response_free(struct http_response *response)
{
	if (!response)
		return;

	free(response->body);
	free(response->error);
	response->body = NULL;
	response->error = NULL;
	response->body_length = 0;
	response->status_code = 0;
}
