#include <sys/types.h>
#include <sys/socket.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <netinet/sctp_uio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ADDRS 8
#define MAX_MESSAGES 16
#define BUF_SIZE 8192

struct message_spec {
	char payload[256];
	uint16_t stream;
	uint32_t ppid;
};

struct options {
	const char *mode;
	char *bind_addrs;
	char *connect_addrs;
	char *subscribe;
	char *messages;
	char *expect_failure;
	int read_messages;
	bool set_nodelay;
	bool emit_local_addrs;
	bool emit_peer_addrs;
	struct sctp_initmsg initmsg;
};

static char fatal_error[256];

static void usage(void);
static int run_server(const struct options *);
static int run_client(const struct options *);

static void
set_fatal_error(const char *message)
{
	strlcpy(fatal_error, message, sizeof(fatal_error));
}

static void
json_escape(FILE *out, const char *value)
{
	const unsigned char *p;

	fputc('"', out);
	for (p = (const unsigned char *)value; *p != '\0'; p++) {
		switch (*p) {
		case '\\':
		case '"':
			fputc('\\', out);
			fputc(*p, out);
			break;
		case '\n':
			fputs("\\n", out);
			break;
		case '\r':
			fputs("\\r", out);
			break;
		case '\t':
			fputs("\\t", out);
			break;
		default:
			fputc(*p, out);
			break;
		}
	}
	fputc('"', out);
}

static void
emit_ready(char addrs[][64], int count)
{
	int i;

	printf("{\"event\":\"ready\",\"local_addrs\":[");
	for (i = 0; i < count; i++) {
		if (i != 0)
			printf(",");
		json_escape(stdout, addrs[i]);
	}
	printf("]}\n");
	fflush(stdout);
}

static void
emit_addrs(const char *event, char addrs[][64], int count)
{
	int i;

	printf("{\"event\":");
	json_escape(stdout, event);
	printf(",\"addrs\":[");
	for (i = 0; i < count; i++) {
		if (i != 0)
			printf(",");
		json_escape(stdout, addrs[i]);
	}
	printf("]}\n");
	fflush(stdout);
}

static void
emit_recv(const char *payload, uint16_t stream, uint32_t ppid, sctp_assoc_t assoc_id)
{
	printf("{\"event\":\"recv\",\"payload\":");
	json_escape(stdout, payload);
	printf(",\"stream\":%u,\"ppid\":%u,\"assoc_id\":%u}\n",
	    (unsigned int)stream, (unsigned int)ppid, (unsigned int)assoc_id);
	fflush(stdout);
}

static void
emit_sent(const char *payload, uint16_t stream, uint32_t ppid)
{
	printf("{\"event\":\"sent\",\"payload\":");
	json_escape(stdout, payload);
	printf(",\"stream\":%u,\"ppid\":%u}\n",
	    (unsigned int)stream, (unsigned int)ppid);
	fflush(stdout);
}

static void
emit_notify(uint16_t type, int flags)
{
	printf("{\"event\":\"notify\",\"type\":%u,\"flags\":%d}\n",
	    (unsigned int)type, flags);
	fflush(stdout);
}

static void
emit_expected_failure(const char *stage, const char *message)
{
	printf("{\"event\":\"expected_failure\",\"stage\":");
	json_escape(stdout, stage);
	printf(",\"message\":");
	json_escape(stdout, message);
	printf("}\n");
	fflush(stdout);
}

static void
emit_complete(int count, const char *field)
{
	printf("{\"event\":\"complete\",\"%s\":%d}\n", field, count);
	fflush(stdout);
}

static void
emit_error(const char *message)
{
	printf("{\"event\":\"error\",\"message\":");
	json_escape(stdout, message);
	printf("}\n");
	fflush(stdout);
}

static char **
split_csv(char *raw, int *count)
{
	char **out, *token;
	int capacity;

	*count = 0;
	if (raw == NULL || *raw == '\0')
		return (NULL);
	capacity = 8;
	out = calloc((size_t)capacity, sizeof(*out));
	if (out == NULL)
		return (NULL);
	for (token = strtok(raw, ","); token != NULL; token = strtok(NULL, ",")) {
		if (*count == capacity) {
			capacity *= 2;
			out = realloc(out, (size_t)capacity * sizeof(*out));
			if (out == NULL)
				return (NULL);
		}
		out[*count] = token;
		(*count)++;
	}
	return (out);
}

static int
parse_addr(const char *raw, struct sockaddr_in *sin)
{
	char host[64];
	char *colon;
	long port;

	if (strlen(raw) >= sizeof(host))
		return (-1);
	strlcpy(host, raw, sizeof(host));
	colon = strrchr(host, ':');
	if (colon == NULL)
		return (-1);
	*colon++ = '\0';
	port = strtol(colon, NULL, 10);
	if (port < 0 || port > 65535)
		return (-1);
	memset(sin, 0, sizeof(*sin));
	sin->sin_len = sizeof(*sin);
	sin->sin_family = AF_INET;
	sin->sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &sin->sin_addr) != 1)
		return (-1);
	return (0);
}

static int
parse_addr_list(char *raw, struct sockaddr_in addrs[MAX_ADDRS], int *count)
{
	char **parts;
	int i, n;

	*count = 0;
	parts = split_csv(raw, &n);
	if (parts == NULL && raw != NULL && *raw != '\0')
		return (-1);
	if (n > MAX_ADDRS) {
		free(parts);
		return (-1);
	}
	for (i = 0; i < n; i++) {
		if (parse_addr(parts[i], &addrs[i]) != 0) {
			free(parts);
			return (-1);
		}
	}
	*count = n;
	free(parts);
	return (0);
}

static int
parse_messages(char *raw, struct message_spec messages[MAX_MESSAGES], int *count)
{
	char **parts;
	char *spec, *stream_str, *ppid_str;
	int i, n;

	*count = 0;
	if (raw == NULL || *raw == '\0')
		return (0);
	parts = split_csv(raw, &n);
	if (parts == NULL)
		return (-1);
	if (n > MAX_MESSAGES) {
		free(parts);
		return (-1);
	}
	for (i = 0; i < n; i++) {
		spec = parts[i];
		stream_str = strchr(spec, ':');
		if (stream_str == NULL) {
			free(parts);
			return (-1);
		}
		*stream_str++ = '\0';
		ppid_str = strchr(stream_str, ':');
		if (ppid_str == NULL) {
			free(parts);
			return (-1);
		}
		*ppid_str++ = '\0';
		strlcpy(messages[i].payload, spec, sizeof(messages[i].payload));
		messages[i].stream = (uint16_t)strtoul(stream_str, NULL, 10);
		messages[i].ppid = (uint32_t)strtoul(ppid_str, NULL, 10);
	}
	*count = n;
	free(parts);
	return (0);
}

static int
apply_subscriptions(int fd, char *raw)
{
	char copy[256];
	char **parts;
	struct sctp_event_subscribe subscribe;
	int i, n, on;

	if (raw == NULL || *raw == '\0')
		goto enable_rcvinfo;
	strlcpy(copy, raw, sizeof(copy));
	parts = split_csv(copy, &n);
	if (parts == NULL)
		return (-1);
	memset(&subscribe, 0, sizeof(subscribe));
	for (i = 0; i < n; i++) {
		if (strcmp(parts[i], "association") == 0)
			subscribe.sctp_association_event = 1;
		else if (strcmp(parts[i], "shutdown") == 0)
			subscribe.sctp_shutdown_event = 1;
		else if (strcmp(parts[i], "dataio") == 0)
			subscribe.sctp_data_io_event = 1;
		else {
			free(parts);
			return (-1);
		}
	}
	free(parts);
	if (setsockopt(fd, IPPROTO_SCTP, SCTP_EVENTS, &subscribe, sizeof(subscribe)) != 0)
		return (-1);
enable_rcvinfo:
	on = 1;
	if (setsockopt(fd, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on, sizeof(on)) != 0)
		return (-1);
	return (0);
}

static int
apply_common_socket_options(int fd, const struct options *opts)
{
	int on;

	if (opts->set_nodelay) {
		on = 1;
		if (setsockopt(fd, IPPROTO_SCTP, SCTP_NODELAY, &on, sizeof(on)) != 0)
			return (-1);
	}
	if (opts->initmsg.sinit_num_ostreams != 0 ||
	    opts->initmsg.sinit_max_instreams != 0 ||
	    opts->initmsg.sinit_max_attempts != 0 ||
	    opts->initmsg.sinit_max_init_timeo != 0) {
		if (setsockopt(fd, IPPROTO_SCTP, SCTP_INITMSG, &opts->initmsg,
		    sizeof(opts->initmsg)) != 0)
			return (-1);
	}
	if (apply_subscriptions(fd, opts->subscribe) != 0)
		return (-1);
	return (0);
}

static int
format_sockaddr(const struct sockaddr *sa, char out[64])
{
	const struct sockaddr_in *sin;
	char host[INET_ADDRSTRLEN];

	if (sa->sa_family != AF_INET)
		return (-1);
	sin = (const struct sockaddr_in *)sa;
	if (inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)) == NULL)
		return (-1);
	snprintf(out, 64, "%s:%u", host, (unsigned int)ntohs(sin->sin_port));
	return (0);
}

static int
load_laddrs(int fd, sctp_assoc_t assoc_id, char out[MAX_ADDRS][64], int *count)
{
	struct sockaddr *addrs, *sa;
	int i, n;

	*count = 0;
	n = sctp_getladdrs(fd, assoc_id, &addrs);
	if (n <= 0)
		return (-1);
	sa = addrs;
	for (i = 0; i < n && i < MAX_ADDRS; i++) {
		if (format_sockaddr(sa, out[i]) != 0) {
			sctp_freeladdrs(addrs);
			return (-1);
		}
		sa = (struct sockaddr *)((char *)sa + sctp_getaddrlen(sa->sa_family));
	}
	*count = (n < MAX_ADDRS) ? n : MAX_ADDRS;
	sctp_freeladdrs(addrs);
	return (0);
}

static int
load_paddrs(int fd, sctp_assoc_t assoc_id, char out[MAX_ADDRS][64], int *count)
{
	struct sockaddr *addrs, *sa;
	int i, n;

	*count = 0;
	n = sctp_getpaddrs(fd, assoc_id, &addrs);
	if (n <= 0)
		return (-1);
	sa = addrs;
	for (i = 0; i < n && i < MAX_ADDRS; i++) {
		if (format_sockaddr(sa, out[i]) != 0) {
			sctp_freepaddrs(addrs);
			return (-1);
		}
		sa = (struct sockaddr *)((char *)sa + sctp_getaddrlen(sa->sa_family));
	}
	*count = (n < MAX_ADDRS) ? n : MAX_ADDRS;
	sctp_freepaddrs(addrs);
	return (0);
}

static int
parse_options(int argc, char **argv, struct options *opts)
{
	int i;

	if (argc < 2)
		return (-1);
	memset(opts, 0, sizeof(*opts));
	opts->mode = argv[1];
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--bind-addrs") == 0 && i + 1 < argc)
			opts->bind_addrs = argv[++i];
		else if (strcmp(argv[i], "--connect-addrs") == 0 && i + 1 < argc)
			opts->connect_addrs = argv[++i];
		else if (strcmp(argv[i], "--subscribe") == 0 && i + 1 < argc)
			opts->subscribe = argv[++i];
		else if (strcmp(argv[i], "--messages") == 0 && i + 1 < argc)
			opts->messages = argv[++i];
		else if (strcmp(argv[i], "--expect-failure") == 0 && i + 1 < argc)
			opts->expect_failure = argv[++i];
		else if (strcmp(argv[i], "--read-messages") == 0 && i + 1 < argc)
			opts->read_messages = atoi(argv[++i]);
		else if (strcmp(argv[i], "--set-nodelay") == 0)
			opts->set_nodelay = true;
		else if (strcmp(argv[i], "--emit-local-addrs") == 0)
			opts->emit_local_addrs = true;
		else if (strcmp(argv[i], "--emit-peer-addrs") == 0)
			opts->emit_peer_addrs = true;
		else if (strcmp(argv[i], "--init-ostreams") == 0 && i + 1 < argc)
			opts->initmsg.sinit_num_ostreams = (uint16_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--init-instreams") == 0 && i + 1 < argc)
			opts->initmsg.sinit_max_instreams = (uint16_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--init-attempts") == 0 && i + 1 < argc)
			opts->initmsg.sinit_max_attempts = (uint16_t)atoi(argv[++i]);
		else if (strcmp(argv[i], "--init-timeout") == 0 && i + 1 < argc)
			opts->initmsg.sinit_max_init_timeo = (uint16_t)atoi(argv[++i]);
		else
			return (-1);
	}
	return (0);
}

static int
run_server(const struct options *opts)
{
	char bind_copy[256];
	char local_addrs[MAX_ADDRS][64];
	char peer_addrs[MAX_ADDRS][64];
	char buf[BUF_SIZE + 1];
	struct sockaddr_in bind_addrs[MAX_ADDRS];
	struct sockaddr_storage from;
	struct iovec iov;
	struct sctp_rcvinfo rcvinfo;
	union sctp_notification *notification;
	socklen_t fromlen, infolen;
	unsigned int infotype;
	int bind_count, fd, flags, local_count, n, peer_count, recv_count;
	sctp_assoc_t assoc_id;

	if (opts->bind_addrs == NULL)
		return (-1);
	strlcpy(bind_copy, opts->bind_addrs, sizeof(bind_copy));
	if (parse_addr_list(bind_copy, bind_addrs, &bind_count) != 0)
		return (-1);
	fd = socket(AF_INET, SOCK_SEQPACKET, IPPROTO_SCTP);
	if (fd < 0)
		return (-1);
	if (apply_common_socket_options(fd, opts) != 0) {
		close(fd);
		return (-1);
	}
	if (bind(fd, (struct sockaddr *)&bind_addrs[0], sizeof(bind_addrs[0])) != 0) {
		close(fd);
		return (-1);
	}
	if (bind_count > 1) {
		struct sockaddr_in extra[MAX_ADDRS];
		struct sockaddr_in actual;
		socklen_t actual_len = sizeof(actual);

		if (getsockname(fd, (struct sockaddr *)&actual, &actual_len) != 0) {
			close(fd);
			return (-1);
		}
		memcpy(extra, &bind_addrs[1], (size_t)(bind_count - 1) * sizeof(extra[0]));
		for (int i = 0; i < bind_count - 1; i++)
			extra[i].sin_port = actual.sin_port;
		if (sctp_bindx(fd, (struct sockaddr *)extra, bind_count - 1, SCTP_BINDX_ADD_ADDR) != 0) {
			close(fd);
			return (-1);
		}
	}
	if (listen(fd, 5) != 0) {
		close(fd);
		return (-1);
	}
	local_count = 0;
	if (load_laddrs(fd, SCTP_FUTURE_ASSOC, local_addrs, &local_count) != 0) {
		struct sockaddr_in actual;
		socklen_t actual_len = sizeof(actual);

		if (getsockname(fd, (struct sockaddr *)&actual, &actual_len) != 0) {
			close(fd);
			return (-1);
		}
		for (int i = 0; i < bind_count; i++) {
			bind_addrs[i].sin_port = actual.sin_port;
			if (format_sockaddr((struct sockaddr *)&bind_addrs[i], local_addrs[i]) != 0) {
				close(fd);
				return (-1);
			}
		}
		local_count = bind_count;
	}
	emit_ready(local_addrs, local_count);

	iov.iov_base = buf;
	iov.iov_len = BUF_SIZE;
	recv_count = 0;
	while (recv_count < opts->read_messages) {
		memset(&from, 0, sizeof(from));
		memset(&rcvinfo, 0, sizeof(rcvinfo));
		fromlen = sizeof(from);
		infolen = sizeof(rcvinfo);
		infotype = SCTP_RECVV_RCVINFO;
		flags = 0;
		n = (int)sctp_recvv(fd, &iov, 1, (struct sockaddr *)&from, &fromlen,
		    &rcvinfo, &infolen, &infotype, &flags);
		if (n < 0) {
			close(fd);
			return (-1);
		}
		if ((flags & MSG_NOTIFICATION) != 0) {
			notification = (union sctp_notification *)(void *)buf;
			emit_notify(notification->sn_header.sn_type, flags);
			continue;
		}
		buf[n] = '\0';
		assoc_id = rcvinfo.rcv_assoc_id;
		emit_recv(buf, rcvinfo.rcv_sid, rcvinfo.rcv_ppid, assoc_id);
		recv_count++;
		if (opts->emit_peer_addrs && recv_count == 1) {
			if (load_paddrs(fd, assoc_id, peer_addrs, &peer_count) == 0)
				emit_addrs("peer_addrs", peer_addrs, peer_count);
		}
	}
	emit_complete(recv_count, "recv_count");
	close(fd);
	return (0);
}

static int
run_client(const struct options *opts)
{
	char connect_copy[256];
	char local_addrs[MAX_ADDRS][64];
	char message_copy[512];
	char peer_addrs[MAX_ADDRS][64];
	struct sockaddr_in connect_addrs[MAX_ADDRS];
	struct iovec iov;
	struct message_spec messages[MAX_MESSAGES];
	struct sctp_sndinfo sndinfo;
	int connect_count, fd, local_count, message_count, n, peer_count;
	sctp_assoc_t assoc_id;

	if (opts->connect_addrs == NULL)
		return (-1);
	strlcpy(connect_copy, opts->connect_addrs, sizeof(connect_copy));
	if (parse_addr_list(connect_copy, connect_addrs, &connect_count) != 0)
		return (-1);
	fd = socket(AF_INET, SOCK_SEQPACKET, IPPROTO_SCTP);
	if (fd < 0)
		return (-1);
	if (apply_common_socket_options(fd, opts) != 0) {
		close(fd);
		return (-1);
	}
	assoc_id = 0;
	if (sctp_connectx(fd, (const struct sockaddr *)connect_addrs, connect_count, &assoc_id) != 0) {
		if (opts->expect_failure != NULL &&
		    (strcmp(opts->expect_failure, "connect") == 0 ||
		    strcmp(opts->expect_failure, "connect_or_send") == 0)) {
			emit_expected_failure("connect", strerror(errno));
			close(fd);
			return (0);
		}
		close(fd);
		return (-1);
	}
	if (opts->expect_failure != NULL && strcmp(opts->expect_failure, "connect") == 0) {
		set_fatal_error("connect succeeded unexpectedly");
		close(fd);
		return (-1);
	}
	if (opts->emit_local_addrs && load_laddrs(fd, assoc_id, local_addrs, &local_count) == 0)
		emit_addrs("local_addrs", local_addrs, local_count);
	if (opts->emit_peer_addrs && load_paddrs(fd, assoc_id, peer_addrs, &peer_count) == 0)
		emit_addrs("peer_addrs", peer_addrs, peer_count);
	if (opts->messages != NULL) {
		strlcpy(message_copy, opts->messages, sizeof(message_copy));
		if (parse_messages(message_copy, messages, &message_count) != 0) {
			close(fd);
			return (-1);
		}
	} else {
		message_count = 0;
	}
	for (int i = 0; i < message_count; i++) {
		memset(&sndinfo, 0, sizeof(sndinfo));
		sndinfo.snd_sid = messages[i].stream;
		sndinfo.snd_ppid = messages[i].ppid;
		sndinfo.snd_assoc_id = assoc_id;
		iov.iov_base = messages[i].payload;
		iov.iov_len = strlen(messages[i].payload);
		n = (int)sctp_sendv(fd, &iov, 1, NULL, 0, &sndinfo, (socklen_t)sizeof(sndinfo),
		    SCTP_SENDV_SNDINFO, 0);
		if (n < 0) {
			if (opts->expect_failure != NULL &&
			    (strcmp(opts->expect_failure, "send") == 0 ||
			    strcmp(opts->expect_failure, "connect_or_send") == 0)) {
				emit_expected_failure("send", strerror(errno));
				close(fd);
				return (0);
			}
			close(fd);
			return (-1);
		}
		emit_sent(messages[i].payload, messages[i].stream, messages[i].ppid);
	}
	if (opts->expect_failure != NULL &&
	    (strcmp(opts->expect_failure, "send") == 0 ||
	    strcmp(opts->expect_failure, "connect_or_send") == 0)) {
		set_fatal_error("send succeeded unexpectedly");
		close(fd);
		return (-1);
	}
	emit_complete(message_count, "sent_count");
	close(fd);
	return (0);
}

static void
usage(void)
{
	fprintf(stderr, "usage: sctp-freebsd-helper server|client [options]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	struct options opts;
	int rc = 0;

	fatal_error[0] = '\0';
	if (parse_options(argc, argv, &opts) != 0)
		usage();
	if (strcmp(opts.mode, "server") == 0)
		rc = run_server(&opts);
	else if (strcmp(opts.mode, "client") == 0)
		rc = run_client(&opts);
	else
		usage();
	if (rc != 0) {
		emit_error(fatal_error[0] != '\0' ? fatal_error : strerror(errno));
		return (1);
	}
	return (0);
}
