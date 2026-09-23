/*
 * wget and curl.
 *
 * Both are ESP-IDF's HTTP client over its TLS (esp-tls on mbedTLS), with
 * the certificate authorities browsers trust built in, so an https://
 * address is checked the way a browser would check it. What is here is the
 * two programs' manners, as their namesakes have them: wget fetches files,
 * with a progress line, and with -c goes on where it left off; curl prints
 * what comes back, and is for talking to things.
 *
 * A TLS handshake is the hungriest thing a program does on this board.
 * mbedTLS keeps its memory in PSRAM (sdkconfig.defaults), but it works on
 * the program's own stack, hence the bigger stacks these two run on.
 *
 * Reads wait a second at most, so that Ctrl-C is noticed, and a transfer
 * gives up after half a minute of silence. Connecting cannot be
 * interrupted, and takes at most CONNECT_MS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"

#include "drivers/drivers.h"
#include "util.h"

#define HTTP_STACK_KB	12
#define MAX_REDIRECTS	10
#define MAX_HEADERS	16
#define CHUNK		4096
#define URL_MAX		1024
#define CONNECT_MS	15000
#define POLL_MS		1000
#define SILENCE_MS	30000
#define PROGRESS_US	200000		/* the progress line, redrawn at most this often */

enum failure {
	F_NONE,
	F_RESOLVE,		/* the name is not known */
	F_CONNECT,		/* nobody answered, or the connection broke */
	F_TLS,			/* the TLS handshake failed */
	F_CERT,			/* ... because the certificate did not check out */
	F_TIMEOUT,
	F_PARTIAL,		/* it ended before all that was promised came */
	F_WRITE,		/* our side: the file could not be written */
	F_INTR,			/* Ctrl-C */
	F_NOMEM,
	F_URL,			/* not an address the client could parse */
};

struct transfer {
	/* what is asked for */
	const char	*prog;
	char		 url[URL_MAX];
	esp_http_client_method_t method;
	const char	*body;
	size_t		 body_len;
	const char	*headers[MAX_HEADERS];	/* "Name: value" */
	int		 nheaders;
	const char	*agent;
	bool		 follow, insecure;
	int64_t		 from;			/* a Range from here: wget -c */
	int		 max_ms;		/* curl -m: 0 for no limit */
	void		(*on_response)(struct transfer *t);	/* each one, redirects too */
	void		*ctx;

	/* what comes back */
	esp_http_client_handle_t client;
	int		 status;
	int64_t		 length;		/* of the body, -1 if not said */
	int64_t		 got;
	char		*head;			/* "Name: value\r\n" for each header */
	size_t		 head_len, head_cap;
	char		 type[64];		/* Content-Type */
	int64_t		 started;
};

/* ------------------------------------------------------------ the client */

static void head_add(struct transfer *t, const char *key, const char *value)
{
	size_t need = strlen(key) + strlen(value) + 5;

	if (t->head_len + need > t->head_cap) {
		size_t cap = (t->head_len + need) * 2;
		char *grown = pt_realloc(t->head, cap);

		if (!grown)
			return;			/* the headers go short, the body does not */
		t->head = grown;
		t->head_cap = cap;
	}
	t->head_len += snprintf(t->head + t->head_len, t->head_cap - t->head_len, "%s: %s\r\n",
				key, value);
}

/* On our own task, from inside the client's calls. */
static esp_err_t on_event(esp_http_client_event_t *e)
{
	struct transfer *t = e->user_data;

	if (e->event_id == HTTP_EVENT_ON_HEADER && t) {
		head_add(t, e->header_key, e->header_value);
		if (!strcasecmp(e->header_key, "Content-Type"))
			strlcpy(t->type, e->header_value, sizeof(t->type));
	}
	return ESP_OK;
}

/* What went wrong under the client, from what esp-tls noted. */
static enum failure why(struct transfer *t, esp_err_t err)
{
	int code = 0, flags = 0;

	if (err == ESP_ERR_HTTP_EAGAIN)
		return F_TIMEOUT;
	esp_http_client_get_and_clear_last_tls_error(t->client, &code, &flags);
	switch (code) {
	case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME:	return F_RESOLVE;
	case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:	return F_TIMEOUT;
	case 0:
	case ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET:
	case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST:	return F_CONNECT;
	}
	return flags ? F_CERT : F_TLS;	/* flags: what the certificate check found */
}

static bool is_redirect(int status)
{
	return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

/*
 * Connects, sends the request and reads the response's headers, following
 * redirects if asked to. The body is then read with body_read().
 */
static enum failure start(struct transfer *t)
{
	esp_http_client_config_t cfg = {
		.url = t->url,
		.method = t->method,
		.timeout_ms = CONNECT_MS,
		.disable_auto_redirect = true,
		.event_handler = on_event,
		.user_data = t,
		.user_agent = t->agent,
		.buffer_size = 2048,
		.buffer_size_tx = 2048,
		.crt_bundle_attach = t->insecure ? NULL : esp_crt_bundle_attach,
		.skip_cert_common_name_check = t->insecure,
	};
	char range[40];

	/* its complaints are ours to make, in words that fit the program */
	esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
	esp_log_level_set("esp-tls", ESP_LOG_NONE);
	esp_log_level_set("esp-tls-mbedtls", ESP_LOG_NONE);
	esp_log_level_set("transport_base", ESP_LOG_NONE);
	esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_NONE);

	t->started = esp_timer_get_time();
	if (!wifi_up())
		return F_CONNECT;
	if (!(t->client = esp_http_client_init(&cfg)))
		return F_URL;
	for (int i = 0; i < t->nheaders; i++) {
		char name[64];
		const char *colon = strchr(t->headers[i], ':'), *value = colon ? colon + 1 : "";
		size_t n = colon ? (size_t)(colon - t->headers[i]) : strlen(t->headers[i]);

		while (*value == ' ')
			value++;
		snprintf(name, sizeof(name), "%.*s", (int)n, t->headers[i]);
		esp_http_client_set_header(t->client, name, value);
	}
	if (t->from > 0) {
		snprintf(range, sizeof(range), "bytes=%lld-", (long long)t->from);
		esp_http_client_set_header(t->client, "Range", range);
	}
	for (int hops = 0;; hops++) {
		esp_err_t err;
		int64_t r;

		t->head_len = 0;
		t->type[0] = '\0';
		if ((err = esp_http_client_open(t->client, t->body_len)))
			return why(t, err);
		if (t->body_len &&
		    esp_http_client_write(t->client, t->body, t->body_len) != (int)t->body_len)
			return why(t, ESP_FAIL);
		if ((r = esp_http_client_fetch_headers(t->client)) < 0)
			return why(t, r == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_HTTP_EAGAIN : ESP_FAIL);
		t->status = esp_http_client_get_status_code(t->client);
		t->length = esp_http_client_is_chunked_response(t->client) ? -1
			  : esp_http_client_get_content_length(t->client);
		if (t->on_response)
			t->on_response(t);
		if (!t->follow || !is_redirect(t->status) || hops == MAX_REDIRECTS ||
		    esp_http_client_set_redirection(t->client) != ESP_OK)
			break;
		esp_http_client_close(t->client);
		/* as browsers do: after a 303, or a 301 or 302 to a POST, a GET */
		if (t->status == 303 || (t->status <= 302 && t->method == HTTP_METHOD_POST)) {
			t->method = HTTP_METHOD_GET;
			t->body_len = 0;
			esp_http_client_set_method(t->client, HTTP_METHOD_GET);
		}
	}
	esp_http_client_get_url(t->client, t->url, sizeof(t->url));
	esp_http_client_set_timeout_ms(t->client, POLL_MS);
	return F_NONE;
}

/* The next piece of the body: its length, 0 at the end, -1 with *fail set. */
static int body_read(struct transfer *t, char *buf, int size, enum failure *fail)
{
	int64_t quiet_since = esp_timer_get_time();

	for (;;) {
		int n;

		if (pt_interrupted()) {
			*fail = F_INTR;
			return -1;
		}
		if (t->max_ms && esp_timer_get_time() - t->started > t->max_ms * 1000LL) {
			*fail = F_TIMEOUT;
			return -1;
		}
		n = esp_http_client_read(t->client, buf, size);
		if (n > 0) {
			t->got += n;
			return n;
		}
		if (n == -ESP_ERR_HTTP_EAGAIN) {
			if (esp_timer_get_time() - quiet_since > SILENCE_MS * 1000LL) {
				*fail = F_TIMEOUT;
				return -1;
			}
			continue;
		}
		if (n == 0 && (t->length < 0 || t->got >= t->length))
			return 0;
		*fail = F_PARTIAL;		/* closed early, or an error: short either way */
		return -1;
	}
}

static void finish(struct transfer *t)
{
	if (t->client) {
		esp_http_client_close(t->client);
		esp_http_client_cleanup(t->client);
		t->client = NULL;
	}
	pt_free(t->head);
	t->head = NULL;
}

/* "example.com" is "http://example.com", as both programs take it. */
static int set_url(struct transfer *t, const char *url)
{
	int n = snprintf(t->url, sizeof(t->url), "%s%s", strstr(url, "://") ? "" : "http://", url);

	return n < (int)sizeof(t->url) ? 0 : -ENAMETOOLONG;
}

/* The host of an address, for messages. */
static const char *host_of(const char *url, char *out, size_t size)
{
	const char *p = strstr(url, "://");
	size_t n;

	p = p ? p + 3 : url;
	n = strcspn(p, "/:?#");
	snprintf(out, size, "%.*s", (int)n, p);
	return out;
}

static const char *reason(int status)
{
	switch (status) {
	case 200: return "OK";
	case 201: return "Created";
	case 204: return "No Content";
	case 206: return "Partial Content";
	case 301: return "Moved Permanently";
	case 302: return "Found";
	case 303: return "See Other";
	case 304: return "Not Modified";
	case 307: return "Temporary Redirect";
	case 308: return "Permanent Redirect";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 416: return "Range Not Satisfiable";
	case 429: return "Too Many Requests";
	case 500: return "Internal Server Error";
	case 502: return "Bad Gateway";
	case 503: return "Service Unavailable";
	case 504: return "Gateway Timeout";
	}
	return "";
}

/* The last part of an address's path, for a file name: index.html for none. */
static void name_of(const char *url, char *out, size_t size)
{
	const char *p = strstr(url, "://"), *end, *slash;

	p = p ? p + 3 : url;
	p += strcspn(p, "/?#");			/* past the host */
	end = p + strcspn(p, "?#");
	for (slash = p; (p = memchr(p, '/', end - p)); slash = ++p)
		;
	if (slash >= end)
		snprintf(out, size, "index.html");
	else
		snprintf(out, size, "%.*s", (int)(end - slash), slash);
}

/* ------------------------------------------------------------ progress */

struct progress {
	const char	*name;
	int64_t		 total;		/* of the whole file, -1 if not known */
	int64_t		 from;		/* what was already there */
	int64_t		 started, drawn;
	bool		 on;
};

static void progress_draw(struct progress *p, int64_t got, bool last)
{
	int64_t now = esp_timer_get_time(), have = p->from + got;
	double secs = (now - p->started) / 1e6;
	char size[16], rate[16], line[96], eta[24] = "";
	int cols = 53;
	struct pt_winsize ws;

	if (!p->on || (!last && now - p->drawn < PROGRESS_US))
		return;
	p->drawn = now;
	if (!pt_ioctl(PT_STDERR, PT_TTY_GETSIZE, &ws) && ws.cols > 20)
		cols = ws.cols;
	human_size(have, size, sizeof(size));
	human_size(secs > 0.1 ? (uint64_t)(got / secs) : 0, rate, sizeof(rate));
	if (p->total > 0 && !last && got > 0 && secs > 0.5)
		snprintf(eta, sizeof(eta), "  eta %llds",
			 (long long)((p->total - have) / (got / secs) + 0.5));
	int tail = p->total > 0
		? snprintf(line, sizeof(line), " %3d%% %6s %6s/s%s", (int)(have * 100 / p->total),
			   size, rate, eta)
		: snprintf(line, sizeof(line), " %6s %6s/s", size, rate);
	int room = cols - 1 - tail;

	if (room < 4)
		room = 4;
	pt_dprintf(PT_STDERR, "\r%-*.*s%s\x1b[K%s", room, room, p->name, line, last ? "\n" : "");
}

/* ------------------------------------------------------------ wget */

static int wget_one(const char *url, const char *out_name, bool quiet, bool resume,
		    const char *agent)
{
	struct transfer t = { .prog = "wget", .method = HTTP_METHOD_GET, .follow = true,
			      .agent = agent };
	struct progress pr = { 0 };
	char name[PT_PATH_MAX], host[64], *buf = NULL;
	struct pt_stat st;
	enum failure bad;
	int fd = -1, status = 0, n;
	bool to_stdout = out_name && !strcmp(out_name, "-");

	if (set_url(&t, url))
		return fail("wget", url, -ENAMETOOLONG);
	host_of(t.url, host, sizeof(host));
	if (out_name) {
		strlcpy(name, out_name, sizeof(name));
	} else {
		name_of(t.url, name, sizeof(name));
		/* as GNU wget does: a file already there makes NAME.1, NAME.2 ... */
		if (!resume && !pt_stat(name, &st)) {
			size_t len = strlen(name);

			for (int k = 1; k < 1000; k++) {
				snprintf(name + len, sizeof(name) - len, ".%d", k);
				if (pt_stat(name, &st))
					break;
			}
		}
	}
	if (resume && !to_stdout && !pt_stat(name, &st) && !st.is_dir)
		t.from = st.size;

	if (!wifi_up()) {
		pt_dprintf(PT_STDERR, "wget: no network: `wifi on`, or `wifi connect`\n");
		return 4;
	}
	if ((bad = start(&t))) {
		switch (bad) {
		case F_RESOLVE:
			pt_dprintf(PT_STDERR, "wget: unable to resolve host address '%s'\n", host);
			break;
		case F_CERT:
			pt_dprintf(PT_STDERR, "wget: cannot verify %s's certificate\n", host);
			finish(&t);
			return 5;
		case F_TLS:
			pt_dprintf(PT_STDERR, "wget: unable to establish a secure connection to %s\n",
				   host);
			finish(&t);
			return 5;
		case F_TIMEOUT:
			pt_dprintf(PT_STDERR, "wget: %s: timed out\n", host);
			break;
		case F_URL:
			pt_dprintf(PT_STDERR, "wget: %s: not an address it can use\n", url);
			finish(&t);
			return 1;
		default:
			pt_dprintf(PT_STDERR, "wget: unable to connect to %s\n", host);
			break;
		}
		finish(&t);
		return 4;
	}
	if (t.from && t.status == 416) {
		if (!quiet)
			pt_dprintf(PT_STDERR, "The file is already fully retrieved; nothing to do.\n");
		finish(&t);
		return 0;
	}
	if (t.status >= 400) {
		pt_dprintf(PT_STDERR, "wget: %s: ERROR %d: %s.\n", url, t.status, reason(t.status));
		finish(&t);
		return 8;
	}
	if (t.from && t.status != 206) {
		/* the server sends it all again: starting over would lose nothing,
		 * but -c was asked for so as not to, and GNU wget refuses too */
		pt_dprintf(PT_STDERR, "wget: %s cannot go on from the middle; "
			   "remove %s to fetch it again\n", host, name);
		finish(&t);
		return 8;
	}

	if (to_stdout)
		fd = PT_STDOUT;
	else if ((fd = pt_open(name, O_WRONLY | O_CREAT | (t.from ? O_APPEND : O_TRUNC))) < 0) {
		fail("wget", name, fd);
		finish(&t);
		return 3;
	}
	pr = (struct progress){ .name = name, .total = t.length >= 0 ? t.from + t.length : -1,
				.from = t.from, .started = esp_timer_get_time(),
				.on = !quiet && pt_isatty(PT_STDERR) };
	if (!(buf = pt_malloc(CHUNK)))
		bad = F_NOMEM;
	while (!bad && (n = body_read(&t, buf, CHUNK, &bad)) > 0) {
		if (write_all(fd, buf, n) < 0)
			bad = F_WRITE;
		progress_draw(&pr, t.got, false);
	}
	progress_draw(&pr, t.got, true);
	pt_free(buf);
	if (fd != PT_STDOUT)
		pt_close(fd);

	switch (bad) {
	case F_NONE:
		if (!quiet)
			pt_dprintf(PT_STDERR, "‘%s’ saved [%lld]\n", to_stdout ? "-" : name,
				   (long long)(t.from + t.got));
		break;
	case F_INTR:
		status = 130;
		break;
	case F_WRITE:
		fail("wget", name, -EIO);
		status = 3;
		break;
	case F_NOMEM:
		fail("wget", NULL, -ENOMEM);
		status = 1;
		break;
	default:
		pt_dprintf(PT_STDERR, "wget: %s: the connection %s after %lld bytes%s\n", host,
			   bad == F_TIMEOUT ? "went quiet" : "broke", (long long)(t.from + t.got),
			   to_stdout ? "" : "; wget -c goes on from there");
		status = 4;
		break;
	}
	finish(&t);
	return status;
}

PT_PROGRAM_STACK(wget, HTTP_STACK_KB, "fetch files from the web\n"
		 "usage: wget [-qc] [-O file] [-U agent] URL...\n"
		 "  -O FILE  save as FILE; - for standard output\n"
		 "  -c  go on from the end of a file partly fetched\n"
		 "  -q  quiet: no progress line\n"
		 "  -U AGENT  what it says it is (User-Agent)\n"
		 "The name is the address's last part (index.html for\n"
		 "none), and NAME.1 if NAME is there. https is checked\n"
		 "against the usual certificate authorities. Needs Wi-Fi.")
{
	struct opt o = { .ind = 1 };
	const char *out = NULL, *agent = "Wget/1.21 (PocketType)";
	bool quiet = false, resume = false;
	int c, status = 0;

	while ((c = getopt_pt(&o, "wget", argc, argv, "O:qcU:")) != -1) {
		switch (c) {
		case 'O': out = o.arg; break;
		case 'q': quiet = true; break;
		case 'c': resume = true; break;
		case 'U': agent = o.arg; break;
		default: return 2;
		}
	}
	if (o.ind == argc) {
		pt_dprintf(PT_STDERR, "wget: missing URL\nusage: wget [-qc] [-O file] URL...\n");
		return 1;
	}
	pt_sigcatch(true);
	for (int i = o.ind; i < argc && status != 130; i++) {
		int s = wget_one(argv[i], out, quiet, resume, agent);

		if (s)
			status = s;
	}
	return status;
}

/* ------------------------------------------------------------ curl */

struct curl {
	bool	 silent, show_errors, fail_http, include, head_only;
	int	 out;
};

static void curl_headers(struct transfer *t)
{
	struct curl *cu = t->ctx;

	if (!cu->include && !cu->head_only)
		return;
	if (cu->fail_http && t->status >= 400)
		return;
	pt_dprintf(cu->out, "HTTP/1.1 %d %s\r\n", t->status, reason(t->status));
	if (t->head)
		write_all(cu->out, t->head, t->head_len);
	write_all(cu->out, "\r\n", 2);
}

/* -w: the format, with %{name} filled in and \n and \t as they are. */
static void curl_write_out(const char *fmt, const struct transfer *t)
{
	char out[512];
	size_t n = 0;
	double secs = (esp_timer_get_time() - t->started) / 1e6;

	while (*fmt && n < sizeof(out) - 64) {
		if (fmt[0] == '\\' && (fmt[1] == 'n' || fmt[1] == 't' || fmt[1] == 'r')) {
			out[n++] = fmt[1] == 'n' ? '\n' : fmt[1] == 't' ? '\t' : '\r';
			fmt += 2;
		} else if (!strncmp(fmt, "%{", 2) && strchr(fmt, '}')) {
			const char *end = strchr(fmt, '}');
			char name[32];

			snprintf(name, sizeof(name), "%.*s", (int)(end - fmt - 2), fmt + 2);
			if (!strcmp(name, "http_code") || !strcmp(name, "response_code"))
				n += snprintf(out + n, sizeof(out) - n, "%03d", t->status);
			else if (!strcmp(name, "size_download"))
				n += snprintf(out + n, sizeof(out) - n, "%lld", (long long)t->got);
			else if (!strcmp(name, "url_effective"))
				n += snprintf(out + n, sizeof(out) - n, "%.200s", t->url);
			else if (!strcmp(name, "content_type"))
				n += snprintf(out + n, sizeof(out) - n, "%s", t->type);
			else if (!strcmp(name, "time_total"))
				n += snprintf(out + n, sizeof(out) - n, "%.6f", secs);
			fmt = end + 1;
		} else {
			out[n++] = *fmt++;
		}
	}
	write_all(PT_STDOUT, out, n);
}

/* -d @file: the file's contents, which the caller frees. */
static char *read_file(const char *path, size_t *len)
{
	int fd = pt_open(path, O_RDONLY);
	size_t cap = 1024;
	char *s = fd >= 0 ? pt_malloc(cap) : NULL;
	ssize_t n;

	*len = 0;
	while (s && (n = pt_read(fd, s + *len, cap - *len)) > 0) {
		*len += n;
		if (*len == cap) {
			char *grown = pt_realloc(s, cap *= 2);

			if (!grown)
				pt_free(s);
			s = grown;
		}
	}
	if (fd >= 0)
		pt_close(fd);
	return s;
}

/* "curl: (6) Could not resolve host: x", unless -s without -S; returns the code. */
static int curl_error(const struct curl *cu, int code, const char *fmt, const char *arg)
{
	char msg[160];

	if (!cu->silent || cu->show_errors) {
		snprintf(msg, sizeof(msg), fmt, arg);
		pt_dprintf(PT_STDERR, "curl: (%d) %s\n", code, msg);
	}
	return code;
}

PT_PROGRAM_STACK(curl, HTTP_STACK_KB, "talk to a web server\n"
		 "usage: curl [options] URL\n"
		 "  -o FILE  write to FILE   -O  to the address's last part\n"
		 "  -L  follow redirects     -I  only the headers (HEAD)\n"
		 "  -i  the headers, then the body\n"
		 "  -s  silent   -S  but errors even so\n"
		 "  -f  an HTTP error is exit 22, and prints nothing\n"
		 "  -k  do not check the server's certificate\n"
		 "  -X METHOD   -d DATA or -d @FILE: send it (POST)\n"
		 "  -H 'Name: value'  a header    -A AGENT  User-Agent\n"
		 "  -m SECONDS  give up after    -w '%{http_code}\\n'  after\n"
		 "Exit 6: no such host, 7: no connection, 28: timed out,\n"
		 "35/60: TLS, or its certificate, failed. Needs Wi-Fi.")
{
	struct transfer t = { .prog = "curl", .method = HTTP_METHOD_GET, .agent = "curl/8.5.0" };
	struct curl cu = { .out = PT_STDOUT };
	struct opt o = { .ind = 1 };
	const char *out_path = NULL, *write_out = NULL, *method = NULL;
	char name[PT_PATH_MAX], host[64], *data = NULL, *buf = NULL;
	size_t data_len = 0;
	bool remote_name = false, has_type = false;
	enum failure bad;
	int c, status = 0, n;

	while ((c = getopt_pt(&o, "curl", argc, argv, "o:OLIisSfkX:d:H:A:m:w:")) != -1) {
		switch (c) {
		case 'o': out_path = o.arg; break;
		case 'O': remote_name = true; break;
		case 'L': t.follow = true; break;
		case 'I': cu.head_only = true; break;
		case 'i': cu.include = true; break;
		case 's': cu.silent = true; break;
		case 'S': cu.show_errors = true; break;
		case 'f': cu.fail_http = true; break;
		case 'k': t.insecure = true; break;
		case 'X': method = o.arg; break;
		case 'A': t.agent = o.arg; break;
		case 'm': t.max_ms = atoi(o.arg) * 1000; break;
		case 'w': write_out = o.arg; break;
		case 'H':
			if (t.nheaders == MAX_HEADERS) {
				pt_free(data);
				return curl_error(&cu, 2, "%s", "too many -H");
			}
			has_type |= !strncasecmp(o.arg, "Content-Type:", 13);
			t.headers[t.nheaders++] = o.arg;
			break;
		case 'd': {
			/* several -d are joined with &, as curl does */
			size_t len;
			char *more = o.arg[0] == '@' ? read_file(o.arg + 1, &len) : NULL;
			const char *piece = o.arg[0] == '@' ? more : o.arg;

			if (o.arg[0] == '@' && !more) {
				pt_free(data);
				return curl_error(&cu, 26, "cannot read %s", o.arg + 1);
			}
			if (o.arg[0] != '@')
				len = strlen(o.arg);
			char *joined = pt_realloc(data, data_len + len + 2);

			if (!joined) {
				pt_free(more);
				pt_free(data);
				return fail("curl", NULL, -ENOMEM);
			}
			data = joined;
			if (data_len)
				data[data_len++] = '&';
			memcpy(data + data_len, piece, len);
			data_len += len;
			data[data_len] = '\0';
			pt_free(more);
			break;
		}
		default:
			pt_free(data);
			return 2;
		}
	}
	if (o.ind != argc - 1) {
		pt_free(data);
		pt_dprintf(PT_STDERR, "curl: %s\nusage: curl [options] URL (help curl)\n",
			   o.ind == argc ? "no URL specified" : "one URL at a time");
		return 2;
	}
	if (set_url(&t, argv[o.ind])) {
		pt_free(data);
		return curl_error(&cu, 3, "URL rejected: %s", "too long");
	}
	host_of(t.url, host, sizeof(host));

	if (data) {
		t.method = HTTP_METHOD_POST;
		t.body = data;
		t.body_len = data_len;
		if (!has_type && t.nheaders < MAX_HEADERS)
			t.headers[t.nheaders++] = "Content-Type: application/x-www-form-urlencoded";
	}
	if (cu.head_only)
		t.method = HTTP_METHOD_HEAD;
	if (method) {
		static const struct { const char *name; esp_http_client_method_t m; } methods[] = {
			{ "GET", HTTP_METHOD_GET }, { "POST", HTTP_METHOD_POST },
			{ "PUT", HTTP_METHOD_PUT }, { "PATCH", HTTP_METHOD_PATCH },
			{ "DELETE", HTTP_METHOD_DELETE }, { "HEAD", HTTP_METHOD_HEAD },
			{ "OPTIONS", HTTP_METHOD_OPTIONS },
		};
		size_t k;

		for (k = 0; k < sizeof(methods) / sizeof(methods[0]); k++)
			if (!strcasecmp(method, methods[k].name))
				break;
		if (k == sizeof(methods) / sizeof(methods[0])) {
			pt_free(data);
			return curl_error(&cu, 2, "a method it does not know: %s", method);
		}
		t.method = methods[k].m;
	}
	if (remote_name) {
		name_of(t.url, name, sizeof(name));
		out_path = name;
	}
	if (out_path && strcmp(out_path, "-") && (cu.out = pt_open(out_path, O_WRONLY | O_CREAT | O_TRUNC)) < 0) {
		pt_free(data);
		fail("curl", out_path, cu.out);
		return 23;
	}
	if (!wifi_up()) {
		status = curl_error(&cu, 6, "Could not resolve host: %s (no network: `wifi on`)",
				    host);
		goto out;
	}

	pt_sigcatch(true);
	t.on_response = curl_headers;
	t.ctx = &cu;
	bad = start(&t);
	switch (bad) {
	case F_NONE:		break;
	case F_RESOLVE:		status = curl_error(&cu, 6, "Could not resolve host: %s", host); goto out;
	case F_TIMEOUT:		status = curl_error(&cu, 28, "Connection to %s timed out", host); goto out;
	case F_TLS:		status = curl_error(&cu, 35, "TLS connect error with %s", host); goto out;
	case F_CERT:		status = curl_error(&cu, 60, "SSL certificate problem: %s's did not check out"
							" (-k to go on anyway)", host); goto out;
	case F_URL:		status = curl_error(&cu, 3, "URL rejected: %s", argv[o.ind]); goto out;
	case F_NOMEM:		status = fail("curl", NULL, -ENOMEM); goto out;
	default:		status = curl_error(&cu, 7, "Failed to connect to %s", host); goto out;
	}
	if (cu.fail_http && t.status >= 400) {
		char code[12];

		snprintf(code, sizeof(code), "%d", t.status);
		status = curl_error(&cu, 22, "The requested URL returned error: %s", code);
		goto out;
	}
	if (!cu.head_only) {
		struct progress pr = { .name = out_path, .total = t.length, .started = esp_timer_get_time(),
				       .on = cu.out != PT_STDOUT && !cu.silent && pt_isatty(PT_STDERR) };

		if (!(buf = pt_malloc(CHUNK))) {
			status = fail("curl", NULL, -ENOMEM);
			goto out;
		}
		while (!bad && (n = body_read(&t, buf, CHUNK, &bad)) > 0) {
			if (write_all(cu.out, buf, n) < 0)
				bad = F_WRITE;
			progress_draw(&pr, t.got, false);
		}
		progress_draw(&pr, t.got, true);
		switch (bad) {
		case F_NONE:	break;
		case F_INTR:	status = 130; break;
		case F_WRITE:	status = curl_error(&cu, 23, "Failure writing output to %s",
						    out_path ? out_path : "stdout"); break;
		case F_TIMEOUT:	status = curl_error(&cu, 28, "Operation timed out: %s", host); break;
		default:	status = curl_error(&cu, 18, "Transfer closed with data still to come: %s",
						    host); break;
		}
	}
	if (write_out && status != 130)
		curl_write_out(write_out, &t);
out:
	finish(&t);
	pt_free(buf);
	pt_free(data);
	if (cu.out != PT_STDOUT)
		pt_close(cu.out);
	return status;
}
