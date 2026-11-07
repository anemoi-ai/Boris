/*
 * http_client.c - libcurl wrapper for HTTP POST and GET requests.
 *
 * Response bodies are accumulated via a libcurl write callback into a
 * growing heap buffer and returned as a single allocation.
 */

#define _POSIX_C_SOURCE 200809L

#include "http_client.h"
#include "logger.h"
#include <curl/curl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

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
	bool too_large; /* set when HTTP_MAX_RESPONSE_BYTES was exceeded */
};

/* Global initialisation state */
static int g_http_initialised = 0;

/* Non-NULL while a caller wants requests to be cancellable. */
static const volatile sig_atomic_t *g_cancel_flag = NULL;

void http_set_cancel_flag(const volatile sig_atomic_t *flag)
{
	g_cancel_flag = flag;
}

/* libcurl progress callback: returning non-zero aborts the transfer. */
static int progress_callback(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
			     curl_off_t ultotal, curl_off_t ulnow)
{
	(void)userdata;
	(void)dltotal;
	(void)dlnow;
	(void)ultotal;
	(void)ulnow;
	return (g_cancel_flag && *g_cancel_flag) ? 1 : 0;
}

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

	if (new_length > HTTP_MAX_RESPONSE_BYTES) {
		buf->too_large = true;
		return 0; /* Abort the transfer */
	}

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
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
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
	buf.too_large = false;
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
		const char *msg = curl_easy_strerror(result);
		if (result == CURLE_ABORTED_BY_CALLBACK)
			msg = "Request cancelled";
		else if (buf.too_large)
			msg = "Response too large";
		response.error = strdup(msg);
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

/* -------------------------------------------------------------------------
 * GET (the fetch tool)
 * ---------------------------------------------------------------------- */

#define HTTP_GET_DEFAULT_TIMEOUT 30
#define HTTP_GET_DEFAULT_MAX	 (4 * 1024 * 1024)
#define HTTP_GET_MAX_REDIRECTS	 5L

static bool ipv4_is_private(uint32_t a) /* host byte order */
{
	return (a >> 24) == 0 ||		 /* 0.0.0.0/8 "this network" */
	       (a >> 24) == 10 ||		 /* 10.0.0.0/8 */
	       (a >> 24) == 127 ||		 /* loopback */
	       (a >> 22) == (100u << 2 | 1) ||	 /* 100.64.0.0/10 carrier NAT */
	       (a >> 16) == (169u << 8 | 254) || /* link-local, cloud metadata */
	       (a >> 20) == (172u << 4 | 1) ||	 /* 172.16.0.0/12 */
	       (a >> 8) == (192u << 16) ||	 /* 192.0.0.0/24 */
	       (a >> 16) == (192u << 8 | 168) || /* 192.168.0.0/16 */
	       (a >> 17) == (198u << 7 | 9) ||	 /* 198.18.0.0/15 benchmarking */
	       (a >> 28) >= 14;			 /* multicast, reserved, broadcast */
}

bool http_address_is_private(const struct sockaddr *addr)
{
	if (!addr)
		return true;
	if (addr->sa_family == AF_INET) {
		const struct sockaddr_in *in = (const struct sockaddr_in *)(const void *)addr;
		return ipv4_is_private(ntohl(in->sin_addr.s_addr));
	}
	if (addr->sa_family != AF_INET6)
		return true;

	const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)(const void *)addr;
	const unsigned char *b = in6->sin6_addr.s6_addr;
	static const unsigned char zeros[12] = {0};
	static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
	if (memcmp(b, mapped, 12) == 0 || memcmp(b, zeros, 12) == 0) {
		/* IPv4-mapped (::ffff:a.b.c.d) or IPv4-compatible, ::, ::1 */
		uint32_t v4 = (uint32_t)b[12] << 24 | (uint32_t)b[13] << 16 |
			      (uint32_t)b[14] << 8 | b[15];
		return ipv4_is_private(v4);
	}
	return (b[0] & 0xfe) == 0xfc ||		    /* fc00::/7 unique local */
	       (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) || /* fe80::/10 link-local */
	       b[0] == 0xff;			    /* multicast */
}

struct get_context {
	struct response_buffer buf;
	size_t max_bytes;
	bool truncated;
	bool allow_private;
	bool blocked;
};

/* Keep up to max_bytes, then stop the transfer (reported as truncated). */
static size_t get_write_callback(void *contents, size_t size, size_t nmemb,
				 void *userdata)
{
	struct get_context *ctx = userdata;
	size_t chunk = size * nmemb;
	size_t room = ctx->max_bytes - ctx->buf.length;
	if (chunk > room) {
		ctx->truncated = true;
		if (room > 0)
			write_callback(contents, 1, room, &ctx->buf);
		return 0;
	}
	return write_callback(contents, size, nmemb, &ctx->buf);
}

/* Refuse connections to non-public addresses (checked after DNS). */
static curl_socket_t get_open_socket(void *clientp, curlsocktype purpose,
				     struct curl_sockaddr *address)
{
	struct get_context *ctx = clientp;
	if (purpose == CURLSOCKTYPE_IPCXN && !ctx->allow_private &&
	    http_address_is_private(&address->addr)) {
		ctx->blocked = true;
		return CURL_SOCKET_BAD;
	}
	return socket(address->family, address->socktype, address->protocol);
}

static char *dup_or_null(const char *s)
{
	return s ? strdup(s) : NULL;
}

struct http_response http_get(const char *url, const struct http_get_options *opts)
{
	struct http_response response;
	memset(&response, 0, sizeof(response));
	if (!url || !opts) {
		response.error = strdup("URL is required");
		return response;
	}
	if (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0) {
		response.error = strdup("Only http:// and https:// URLs are supported");
		return response;
	}
	if (!g_http_initialised)
		http_global_init();

	CURL *curl = curl_easy_init();
	if (!curl) {
		response.error = strdup("Failed to initialize libcurl");
		return response;
	}

	struct get_context ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.max_bytes = opts->max_bytes ? opts->max_bytes : HTTP_GET_DEFAULT_MAX;
	if (ctx.max_bytes > HTTP_MAX_RESPONSE_BYTES)
		ctx.max_bytes = HTTP_MAX_RESPONSE_BYTES;
	ctx.allow_private = opts->allow_private;
	ctx.buf.data = malloc(4096);
	ctx.buf.capacity = 4096;
	if (!ctx.buf.data) {
		response.error = strdup("Out of memory");
		curl_easy_cleanup(curl);
		return response;
	}
	ctx.buf.data[0] = '\0';

	curl_setup_common(curl, url,
			  opts->timeout_secs > 0 ? opts->timeout_secs : HTTP_GET_DEFAULT_TIMEOUT,
			  &ctx.buf, opts->verify_ssl);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, get_write_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
	curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, get_open_socket);
	curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, &ctx);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, HTTP_GET_MAX_REDIRECTS);
	curl_easy_setopt(curl, CURLOPT_PROXY, ""); /* no proxy: keep the address check */
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, opts->user_agent ? opts->user_agent : "Boris");
#if LIBCURL_VERSION_NUM >= 0x075500 /* 7.85.0 */
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
	struct curl_slist *headers = curl_slist_append(
		NULL, "Accept: text/html, text/plain, application/json, */*;q=0.5");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	CURLcode rc = curl_easy_perform(curl);
	bool cut_short = rc == CURLE_WRITE_ERROR && ctx.truncated;

	if (rc != CURLE_OK && !cut_short) {
		if (ctx.blocked)
			response.error = strdup("Refused: the host resolves to a loopback, "
						"private or link-local address");
		else if (rc == CURLE_ABORTED_BY_CALLBACK)
			response.error = strdup("Request cancelled");
		else
			response.error = strdup(curl_easy_strerror(rc));
	} else {
		char *ct = NULL, *eff = NULL;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
		curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
		curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &eff);
		response.content_type = dup_or_null(ct);
		response.effective_url = dup_or_null(eff);
		response.truncated = ctx.truncated;
		response.body = ctx.buf.data;
		response.body_length = ctx.buf.length;
		ctx.buf.data = NULL;
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(ctx.buf.data);
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
	free(response->content_type);
	free(response->effective_url);
	memset(response, 0, sizeof(*response));
}
