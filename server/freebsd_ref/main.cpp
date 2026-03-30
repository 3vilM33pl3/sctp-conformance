#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <netinet/sctp_uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::system_clock;

constexpr int kBacklog = 16;
constexpr size_t kBufferSize = 8192;
struct MessageSpec {
	std::string payload;
	uint16_t stream = 0;
	uint32_t ppid = 0;
};

enum class CompletionMode {
	ServerObserved,
	AgentReported,
	Hybrid,
};

enum class ScenarioKind {
	AgentOnly,
	ReceiveMessages,
	SendAfterTrigger,
};

enum class FeatureState {
	Pending,
	Active,
	Passed,
	Failed,
	Unsupported,
	TimedOut,
};

struct FeatureDefinition {
	std::string id;
	std::string title;
	std::string category;
	std::string summary;
	std::string instructions_text;
	CompletionMode completion_mode = CompletionMode::ServerObserved;
	ScenarioKind scenario_kind = ScenarioKind::AgentOnly;
	int timeout_seconds = 20;
	size_t bind_address_count = 1;
	std::vector<std::string> client_socket_options;
	std::vector<std::string> client_subscriptions;
	std::vector<MessageSpec> client_send_messages;
	std::vector<MessageSpec> server_send_messages;
	std::string report_prompt;
	std::string trigger_payload;
	std::string negative_connect_target;
	size_t expected_peer_addr_count = 0;
};

struct FeatureExecution {
	explicit FeatureExecution(const FeatureDefinition* feature)
	    : definition(feature)
	{
	}

	const FeatureDefinition* definition = nullptr;
	FeatureState state = FeatureState::Pending;
	std::string message = "not started";
	std::string evidence_kind;
	std::string evidence_text;
	std::string report_text;
	std::string contract_json;
	bool network_complete = false;
	bool agent_complete = false;
	int active_fd = -1;
	Clock::time_point started_at {};
	Clock::time_point deadline_at {};
	Clock::time_point finished_at {};
	std::mutex mutex;
};

struct Session {
	std::string id;
	std::string agent_name;
	std::string environment_name;
	Clock::time_point created_at {};
	std::unordered_map<std::string, std::shared_ptr<FeatureExecution>> features;
	std::string active_feature_id;
	std::mutex mutex;
};

struct ServerOptions {
	std::string http_host = "0.0.0.0";
	uint16_t http_port = 8080;
	std::vector<std::string> sctp_bind_addrs = {"127.0.0.1", "127.0.0.2"};
	std::vector<std::string> sctp_advertise_addrs;
};

struct HttpRequest {
	std::string method;
	std::string path;
	std::string body;
	std::map<std::string, std::string> headers;
};

struct HttpResponse {
	int status = 200;
	std::string body;
	std::string content_type = "application/json";
	std::vector<std::pair<std::string, std::string>> headers;
};

struct ListeningSocket {
	int fd = -1;
	std::vector<std::string> bind_addrs;
	std::vector<std::string> advertise_addrs;
};

[[nodiscard]] std::string
trim(const std::string& value)
{
	size_t start = 0;
	while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
		start++;
	size_t end = value.size();
	while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
		end--;
	return value.substr(start, end - start);
}

[[nodiscard]] std::vector<std::string>
split(const std::string& value, char delimiter)
{
	std::vector<std::string> out;
	std::string current;
	std::istringstream input(value);
	while (std::getline(input, current, delimiter)) {
		current = trim(current);
		if (!current.empty())
			out.push_back(current);
	}
	return out;
}

[[nodiscard]] std::string
join(const std::vector<std::string>& values, const std::string& delimiter)
{
	std::ostringstream out;
	for (size_t i = 0; i < values.size(); i++) {
		if (i != 0)
			out << delimiter;
		out << values[i];
	}
	return out.str();
}

[[nodiscard]] std::string
json_escape(const std::string& value)
{
	std::ostringstream out;
	for (unsigned char ch : value) {
		switch (ch) {
		case '\\':
			out << "\\\\";
			break;
		case '"':
			out << "\\\"";
			break;
		case '\n':
			out << "\\n";
			break;
		case '\r':
			out << "\\r";
			break;
		case '\t':
			out << "\\t";
			break;
		default:
			if (ch < 0x20) {
				out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
				    << static_cast<unsigned int>(ch) << std::dec << std::setfill(' ');
			} else {
				out << static_cast<char>(ch);
			}
			break;
		}
	}
	return out.str();
}

[[nodiscard]] std::string
json_quote(const std::string& value)
{
	return "\"" + json_escape(value) + "\"";
}

[[nodiscard]] const char*
to_string(CompletionMode mode)
{
	switch (mode) {
	case CompletionMode::ServerObserved:
		return "server_observed";
	case CompletionMode::AgentReported:
		return "agent_reported";
	case CompletionMode::Hybrid:
		return "hybrid";
	}
	return "unknown";
}

[[nodiscard]] const char*
to_string(FeatureState state)
{
	switch (state) {
	case FeatureState::Pending:
		return "pending";
	case FeatureState::Active:
		return "active";
	case FeatureState::Passed:
		return "passed";
	case FeatureState::Failed:
		return "failed";
	case FeatureState::Unsupported:
		return "unsupported";
	case FeatureState::TimedOut:
		return "timed_out";
	}
	return "unknown";
}

[[nodiscard]] std::string
http_status_text(int status)
{
	switch (status) {
	case 200:
		return "OK";
	case 201:
		return "Created";
	case 400:
		return "Bad Request";
	case 404:
		return "Not Found";
	case 409:
		return "Conflict";
	case 500:
		return "Internal Server Error";
	default:
		return "Status";
	}
}

[[nodiscard]] std::string
iso_time(const Clock::time_point& point)
{
	if (point.time_since_epoch().count() == 0)
		return "";
	const std::time_t seconds = Clock::to_time_t(point);
	std::tm tm_value {};
	gmtime_r(&seconds, &tm_value);
	std::ostringstream out;
	out << std::put_time(&tm_value, "%Y-%m-%dT%H:%M:%SZ");
	return out.str();
}

[[nodiscard]] std::string
json_array_strings(const std::vector<std::string>& values)
{
	std::ostringstream out;
	out << "[";
	for (size_t i = 0; i < values.size(); i++) {
		if (i != 0)
			out << ",";
		out << json_quote(values[i]);
	}
	out << "]";
	return out.str();
}

[[nodiscard]] std::string
json_messages(const std::vector<MessageSpec>& values)
{
	std::ostringstream out;
	out << "[";
	for (size_t i = 0; i < values.size(); i++) {
		if (i != 0)
			out << ",";
		out << "{"
		    << "\"payload\":" << json_quote(values[i].payload) << ","
		    << "\"stream\":" << values[i].stream << ","
		    << "\"ppid\":" << values[i].ppid
		    << "}";
	}
	out << "]";
	return out.str();
}

[[nodiscard]] std::string
json_error(const std::string& message)
{
	return std::string("{\"error\":") + json_quote(message) + "}";
}

[[nodiscard]] std::string
html_escape(const std::string& value)
{
	std::ostringstream out;
	for (char ch : value) {
		switch (ch) {
		case '&':
			out << "&amp;";
			break;
		case '<':
			out << "&lt;";
			break;
		case '>':
			out << "&gt;";
			break;
		case '"':
			out << "&quot;";
			break;
		case '\'':
			out << "&#39;";
			break;
		default:
			out << ch;
			break;
		}
	}
	return out.str();
}

bool
write_all(int fd, const std::string& data)
{
	size_t written = 0;
	while (written < data.size()) {
		ssize_t rc = write(fd, data.data() + written, data.size() - written);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (rc == 0)
			return false;
		written += static_cast<size_t>(rc);
	}
	return true;
}

bool
append_utf8_codepoint(uint32_t codepoint, std::ostringstream& buffer)
{
	if (codepoint <= 0x7F) {
		buffer << static_cast<char>(codepoint);
		return true;
	}
	if (codepoint <= 0x7FF) {
		buffer << static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
		buffer << static_cast<char>(0x80 | (codepoint & 0x3F));
		return true;
	}
	if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
		return false;
	if (codepoint <= 0xFFFF) {
		buffer << static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
		buffer << static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		buffer << static_cast<char>(0x80 | (codepoint & 0x3F));
		return true;
	}
	if (codepoint <= 0x10FFFF) {
		buffer << static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07));
		buffer << static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
		buffer << static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
		buffer << static_cast<char>(0x80 | (codepoint & 0x3F));
		return true;
	}
	return false;
}

struct JsonValue {
	enum class Type {
		String,
		Object,
		Array,
	};

	Type type = Type::String;
	std::string string_value;
	std::map<std::string, JsonValue> object_value;
	std::vector<JsonValue> array_value;
};

void
skip_json_ws(const std::string& input, size_t& i)
{
	while (i < input.size() && std::isspace(static_cast<unsigned char>(input[i])))
		i++;
}

bool
parse_json_string_value(const std::string& input, size_t& i, std::string& out_value, std::string& error)
{
	if (i >= input.size() || input[i] != '"') {
		error = "expected JSON string";
		return false;
	}
	i++;
	std::ostringstream buffer;
	while (i < input.size()) {
		char ch = input[i++];
		if (ch == '"') {
			out_value = buffer.str();
			return true;
		}
		if (ch == '\\') {
			if (i >= input.size()) {
				error = "invalid JSON escape";
				return false;
			}
			char esc = input[i++];
			switch (esc) {
			case '"':
			case '\\':
			case '/':
				buffer << esc;
				break;
			case 'b':
				buffer << '\b';
				break;
			case 'f':
				buffer << '\f';
				break;
			case 'n':
				buffer << '\n';
				break;
			case 'r':
				buffer << '\r';
				break;
			case 't':
				buffer << '\t';
				break;
			case 'u': {
				if (i + 4 > input.size()) {
					error = "invalid JSON unicode escape";
					return false;
				}
				uint32_t codepoint = 0;
				for (size_t hex = 0; hex < 4; ++hex) {
					char digit = input[i++];
					codepoint <<= 4;
					if (digit >= '0' && digit <= '9')
						codepoint |= static_cast<uint32_t>(digit - '0');
					else if (digit >= 'a' && digit <= 'f')
						codepoint |= static_cast<uint32_t>(digit - 'a' + 10);
					else if (digit >= 'A' && digit <= 'F')
						codepoint |= static_cast<uint32_t>(digit - 'A' + 10);
					else {
						error = "invalid JSON unicode escape";
						return false;
					}
				}
				if (!append_utf8_codepoint(codepoint, buffer)) {
					error = "unsupported JSON unicode escape";
					return false;
				}
				break;
			}
			default:
				error = "unsupported JSON escape";
				return false;
			}
		} else {
			buffer << ch;
		}
	}
	error = "unterminated JSON string";
	return false;
}

bool parse_json_value(const std::string& input, size_t& i, JsonValue& out, std::string& error);

bool
parse_json_object_value(const std::string& input, size_t& i, JsonValue& out, std::string& error)
{
	skip_json_ws(input, i);
	if (i >= input.size() || input[i] != '{') {
		error = "expected JSON object";
		return false;
	}
	out.type = JsonValue::Type::Object;
	out.object_value.clear();
	i++;
	skip_json_ws(input, i);
	if (i < input.size() && input[i] == '}') {
		i++;
		return true;
	}
	while (i < input.size()) {
		std::string key;
		JsonValue value;
		skip_json_ws(input, i);
		if (!parse_json_string_value(input, i, key, error))
			return false;
		skip_json_ws(input, i);
		if (i >= input.size() || input[i] != ':') {
			error = "expected ':'";
			return false;
		}
		i++;
		if (!parse_json_value(input, i, value, error))
			return false;
		out.object_value[key] = value;
		skip_json_ws(input, i);
		if (i >= input.size()) {
			error = "unterminated JSON object";
			return false;
		}
		if (input[i] == '}') {
			i++;
			return true;
		}
		if (input[i] != ',') {
			error = "expected ','";
			return false;
		}
		i++;
	}
	error = "unterminated JSON object";
	return false;
}

bool
parse_json_array_value(const std::string& input, size_t& i, JsonValue& out, std::string& error)
{
	skip_json_ws(input, i);
	if (i >= input.size() || input[i] != '[') {
		error = "expected JSON array";
		return false;
	}
	out.type = JsonValue::Type::Array;
	out.array_value.clear();
	i++;
	skip_json_ws(input, i);
	if (i < input.size() && input[i] == ']') {
		i++;
		return true;
	}
	while (i < input.size()) {
		JsonValue value;
		if (!parse_json_value(input, i, value, error))
			return false;
		out.array_value.push_back(std::move(value));
		skip_json_ws(input, i);
		if (i >= input.size()) {
			error = "unterminated JSON array";
			return false;
		}
		if (input[i] == ']') {
			i++;
			return true;
		}
		if (input[i] != ',') {
			error = "expected ','";
			return false;
		}
		i++;
	}
	error = "unterminated JSON array";
	return false;
}

bool
parse_json_value(const std::string& input, size_t& i, JsonValue& out, std::string& error)
{
	skip_json_ws(input, i);
	if (i >= input.size()) {
		error = "expected JSON value";
		return false;
	}
	if (input[i] == '"') {
		out.type = JsonValue::Type::String;
		out.object_value.clear();
		out.array_value.clear();
		return parse_json_string_value(input, i, out.string_value, error);
	}
	if (input[i] == '{')
		return parse_json_object_value(input, i, out, error);
	if (input[i] == '[')
		return parse_json_array_value(input, i, out, error);
	error = "unsupported JSON value";
	return false;
}

bool
parse_json_root_object(const std::string& input, JsonValue& out, std::string& error)
{
	size_t i = 0;
	if (!parse_json_object_value(input, i, out, error))
		return false;
	skip_json_ws(input, i);
	if (i != input.size()) {
		error = "unexpected trailing JSON content";
		return false;
	}
	return true;
}

bool
parse_simple_json_object(const std::string& input, std::map<std::string, std::string>& out, std::string& error)
{
	JsonValue root;
	if (!parse_json_root_object(input, root, error))
		return false;
	out.clear();
	for (const auto& [key, value] : root.object_value) {
		if (value.type != JsonValue::Type::String) {
			error = "expected string JSON value";
			return false;
		}
		out[key] = value.string_value;
	}
	return true;
}

[[nodiscard]] std::string
format_sockaddr(const struct sockaddr* address)
{
	if (address == nullptr || address->sa_family != AF_INET)
		return "";
	char host[INET_ADDRSTRLEN] = {0};
	const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(address);
	if (inet_ntop(AF_INET, &sin->sin_addr, host, sizeof(host)) == nullptr)
		return "";
	return std::string(host) + ":" + std::to_string(ntohs(sin->sin_port));
}

[[nodiscard]] std::vector<std::string>
load_peer_addrs(int fd, sctp_assoc_t assoc_id)
{
	std::vector<std::string> out;
	struct sockaddr* addrs = nullptr;
	const int count = sctp_getpaddrs(fd, assoc_id, &addrs);
	if (count <= 0 || addrs == nullptr)
		return out;
	struct sockaddr* current = addrs;
	for (int i = 0; i < count; i++) {
		std::string rendered = format_sockaddr(current);
		if (!rendered.empty())
			out.push_back(rendered);
		current = reinterpret_cast<struct sockaddr*>(
		    reinterpret_cast<char*>(current) + sctp_getaddrlen(current->sa_family));
	}
	sctp_freepaddrs(addrs);
	return out;
}

[[nodiscard]] std::string
strerror_string(int value)
{
	return std::string(std::strerror(value));
}

bool
set_socket_timeout(int fd, int seconds, std::string& error)
{
	struct timeval timeout_value {};
	timeout_value.tv_sec = seconds;
	timeout_value.tv_usec = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout_value, sizeof(timeout_value)) != 0) {
		error = strerror_string(errno);
		return false;
	}
	return true;
}

bool
set_http_timeout(int fd, int seconds, std::string& error)
{
	struct timeval timeout_value {};
	timeout_value.tv_sec = seconds;
	timeout_value.tv_usec = 0;
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout_value, sizeof(timeout_value)) != 0) {
		error = strerror_string(errno);
		return false;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout_value, sizeof(timeout_value)) != 0) {
		error = strerror_string(errno);
		return false;
	}
	return true;
}

bool
apply_server_subscriptions(int fd, std::string& error)
{
	struct sctp_event_subscribe subscribe {};
	int on = 1;
	subscribe.sctp_association_event = 1;
	subscribe.sctp_shutdown_event = 1;
	subscribe.sctp_data_io_event = 1;
	if (setsockopt(fd, IPPROTO_SCTP, SCTP_EVENTS, &subscribe, sizeof(subscribe)) != 0) {
		error = strerror_string(errno);
		return false;
	}
	if (setsockopt(fd, IPPROTO_SCTP, SCTP_RECVRCVINFO, &on, sizeof(on)) != 0) {
		error = strerror_string(errno);
		return false;
	}
	return true;
}

bool
apply_server_initmsg(int fd, std::string& error)
{
	struct sctp_initmsg initmsg {};
	initmsg.sinit_num_ostreams = 32;
	initmsg.sinit_max_instreams = 32;
	if (setsockopt(fd, IPPROTO_SCTP, SCTP_INITMSG, &initmsg, sizeof(initmsg)) != 0) {
		error = strerror_string(errno);
		return false;
	}
	return true;
}

[[nodiscard]] std::vector<std::string>
with_port(const std::vector<std::string>& addrs, uint16_t port)
{
	std::vector<std::string> out;
	out.reserve(addrs.size());
	for (const std::string& addr : addrs)
		out.push_back(addr + ":" + std::to_string(port));
	return out;
}

[[nodiscard]] std::string
missing_bind_message(const std::string& address)
{
	if (address == "127.0.0.2")
		return "address 127.0.0.2 is not configured on the FreeBSD host; run `ifconfig lo0 alias 127.0.0.2/8` as root";
	return "address " + address + " is not configured on the FreeBSD host; add it to a local interface or change --sctp-addrs";
}

std::optional<ListeningSocket>
create_listening_socket(const ServerOptions& options, size_t bind_count, int timeout_seconds, std::string& error)
{
	if (options.sctp_bind_addrs.size() < bind_count) {
		error = "feature requires " + std::to_string(bind_count) +
		    " SCTP addresses; restart the server with --sctp-addrs containing enough local addresses";
		return std::nullopt;
	}
	std::vector<std::string> advertise = options.sctp_advertise_addrs.empty()
	    ? options.sctp_bind_addrs
	    : options.sctp_advertise_addrs;
	if (advertise.size() < bind_count) {
		error = "feature requires " + std::to_string(bind_count) +
		    " advertised SCTP addresses; restart the server with --advertise-addrs containing enough addresses";
		return std::nullopt;
	}

	int fd = socket(AF_INET, SOCK_SEQPACKET, IPPROTO_SCTP);
	if (fd < 0) {
		error = strerror_string(errno);
		return std::nullopt;
	}
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	if (!apply_server_initmsg(fd, error) || !apply_server_subscriptions(fd, error) || !set_socket_timeout(fd, timeout_seconds, error)) {
		close(fd);
		return std::nullopt;
	}

	std::vector<struct sockaddr_in> bind_addrs;
	bind_addrs.reserve(bind_count);
	for (size_t i = 0; i < bind_count; i++) {
		struct sockaddr_in addr {};
		addr.sin_len = sizeof(addr);
		addr.sin_family = AF_INET;
		addr.sin_port = htons(0);
		if (inet_pton(AF_INET, options.sctp_bind_addrs[i].c_str(), &addr.sin_addr) != 1) {
			close(fd);
			error = "invalid IPv4 bind address " + options.sctp_bind_addrs[i];
			return std::nullopt;
		}
		bind_addrs.push_back(addr);
	}
	if (bind(fd, reinterpret_cast<struct sockaddr*>(&bind_addrs[0]), sizeof(bind_addrs[0])) != 0) {
		const int bind_errno = errno;
		close(fd);
		error = bind_errno == EADDRNOTAVAIL
		    ? missing_bind_message(options.sctp_bind_addrs[0])
		    : strerror_string(bind_errno);
		return std::nullopt;
	}
	if (bind_count > 1) {
		struct sockaddr_in actual {};
		socklen_t actual_len = sizeof(actual);
		if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&actual), &actual_len) != 0) {
			error = strerror_string(errno);
			close(fd);
			return std::nullopt;
		}
		std::vector<struct sockaddr_in> extra;
		for (size_t i = 1; i < bind_count; i++) {
			bind_addrs[i].sin_port = actual.sin_port;
			extra.push_back(bind_addrs[i]);
		}
		if (sctp_bindx(fd, reinterpret_cast<struct sockaddr*>(extra.data()), static_cast<int>(extra.size()), SCTP_BINDX_ADD_ADDR) != 0) {
			const int bind_errno = errno;
			close(fd);
			error = bind_errno == EADDRNOTAVAIL
			    ? missing_bind_message(options.sctp_bind_addrs[1])
			    : strerror_string(bind_errno);
			return std::nullopt;
		}
	}
	if (listen(fd, 8) != 0) {
		error = strerror_string(errno);
		close(fd);
		return std::nullopt;
	}

	struct sockaddr_in actual {};
	socklen_t actual_len = sizeof(actual);
	if (getsockname(fd, reinterpret_cast<struct sockaddr*>(&actual), &actual_len) != 0) {
		error = strerror_string(errno);
		close(fd);
		return std::nullopt;
	}
	ListeningSocket socket_info;
	socket_info.fd = fd;
	socket_info.bind_addrs = with_port(
	    std::vector<std::string>(options.sctp_bind_addrs.begin(), options.sctp_bind_addrs.begin() + static_cast<long>(bind_count)),
	    ntohs(actual.sin_port));
	socket_info.advertise_addrs = with_port(
	    std::vector<std::string>(advertise.begin(), advertise.begin() + static_cast<long>(bind_count)),
	    ntohs(actual.sin_port));
	return socket_info;
}

void
clear_active_feature(const std::shared_ptr<Session>& session, const std::string& feature_id)
{
	std::lock_guard<std::mutex> session_guard(session->mutex);
	if (session->active_feature_id == feature_id)
		session->active_feature_id.clear();
}

void
set_active_fd(const std::shared_ptr<FeatureExecution>& execution, int fd)
{
	std::lock_guard<std::mutex> lock(execution->mutex);
	execution->active_fd = fd;
}

[[nodiscard]] int
take_active_fd(const std::shared_ptr<FeatureExecution>& execution)
{
	std::lock_guard<std::mutex> lock(execution->mutex);
	const int fd = execution->active_fd;
	execution->active_fd = -1;
	return fd;
}

void
close_active_fd(const std::shared_ptr<FeatureExecution>& execution)
{
	const int fd = take_active_fd(execution);
	if (fd >= 0)
		close(fd);
}

void
mark_failed(const std::shared_ptr<Session>& session, const std::shared_ptr<FeatureExecution>& execution, const std::string& message)
{
	{
		std::lock_guard<std::mutex> lock(execution->mutex);
		if (execution->state != FeatureState::Active)
			return;
		execution->state = FeatureState::Failed;
		execution->message = message;
		execution->finished_at = Clock::now();
	}
	close_active_fd(execution);
	clear_active_feature(session, execution->definition->id);
}

void
mark_network_complete(const std::shared_ptr<Session>& session, const std::shared_ptr<FeatureExecution>& execution)
{
	bool finished = false;
	{
		std::lock_guard<std::mutex> lock(execution->mutex);
		if (execution->state != FeatureState::Active)
			return;
		execution->network_complete = true;
		if (execution->definition->completion_mode == CompletionMode::ServerObserved || execution->agent_complete) {
			execution->state = FeatureState::Passed;
			execution->message = "scenario completed successfully";
			execution->finished_at = Clock::now();
			finished = true;
		} else {
			execution->message = "server observed SCTP behavior; waiting for client completion report";
		}
	}
	close_active_fd(execution);
	if (finished)
		clear_active_feature(session, execution->definition->id);
}

void
maybe_timeout(const std::shared_ptr<Session>& session, const std::shared_ptr<FeatureExecution>& execution)
{
	bool finished = false;
	{
		std::lock_guard<std::mutex> lock(execution->mutex);
		if (execution->state != FeatureState::Active)
			return;
		if (Clock::now() < execution->deadline_at)
			return;
		execution->state = FeatureState::TimedOut;
		execution->message = "scenario timed out";
		execution->finished_at = Clock::now();
		finished = true;
	}
	close_active_fd(execution);
	if (finished)
		clear_active_feature(session, execution->definition->id);
}

void
run_receive_worker(
    const std::shared_ptr<Session>& session,
    const std::shared_ptr<FeatureExecution>& execution,
    int fd,
    std::vector<MessageSpec> expected_messages,
    size_t expected_peer_addr_count)
{
	std::thread([session, execution, fd, expected_messages = std::move(expected_messages), expected_peer_addr_count]() {
		char buffer[kBufferSize + 1] = {0};
		struct iovec iov {};
		iov.iov_base = buffer;
		iov.iov_len = kBufferSize;
		size_t matched = 0;
		bool checked_peer_addrs = false;
		while (matched < expected_messages.size()) {
			struct sockaddr_storage from {};
			struct sctp_rcvinfo rcvinfo {};
			socklen_t fromlen = sizeof(from);
			socklen_t infolen = sizeof(rcvinfo);
			unsigned int infotype = SCTP_RECVV_RCVINFO;
			int flags = 0;
			const int received = static_cast<int>(sctp_recvv(
			    fd,
			    &iov,
			    1,
			    reinterpret_cast<struct sockaddr*>(&from),
			    &fromlen,
			    &rcvinfo,
			    &infolen,
			    &infotype,
			    &flags));
			if (received < 0) {
				const int recv_errno = errno;
				mark_failed(session, execution, recv_errno == EWOULDBLOCK || recv_errno == EAGAIN
				    ? "timed out waiting for SCTP client traffic"
				    : strerror_string(recv_errno));
				return;
			}
			if ((flags & MSG_NOTIFICATION) != 0)
				continue;
			buffer[received] = '\0';
			const MessageSpec& expected = expected_messages[matched];
			if (expected.payload != buffer) {
				mark_failed(session, execution, "unexpected payload for feature " + execution->definition->id);
				return;
			}
			if (expected.stream != rcvinfo.rcv_sid) {
				mark_failed(session, execution, "unexpected SCTP stream id");
				return;
			}
			if (expected.ppid != rcvinfo.rcv_ppid) {
				mark_failed(session, execution, "unexpected SCTP PPID");
				return;
			}
			matched++;
			if (!checked_peer_addrs && expected_peer_addr_count > 0) {
				checked_peer_addrs = true;
				std::vector<std::string> peer_addrs = load_peer_addrs(fd, rcvinfo.rcv_assoc_id);
				if (peer_addrs.size() < expected_peer_addr_count) {
					mark_failed(session, execution, "peer address enumeration did not return enough addresses");
					return;
				}
			}
		}
		mark_network_complete(session, execution);
	}).detach();
}

void
run_send_after_trigger_worker(
    const std::shared_ptr<Session>& session,
    const std::shared_ptr<FeatureExecution>& execution,
    int fd,
    std::string trigger_payload,
    std::vector<MessageSpec> server_messages)
{
	std::thread([session, execution, fd, trigger_payload = std::move(trigger_payload), server_messages = std::move(server_messages)]() {
		char buffer[kBufferSize + 1] = {0};
		struct iovec iov {};
		iov.iov_base = buffer;
		iov.iov_len = kBufferSize;
		struct sockaddr_storage from {};
		struct sctp_rcvinfo rcvinfo {};
		socklen_t fromlen = sizeof(from);
		socklen_t infolen = sizeof(rcvinfo);
		unsigned int infotype = SCTP_RECVV_RCVINFO;
		int flags = 0;
		while (true) {
			const int received = static_cast<int>(sctp_recvv(
			    fd,
			    &iov,
			    1,
			    reinterpret_cast<struct sockaddr*>(&from),
			    &fromlen,
			    &rcvinfo,
			    &infolen,
			    &infotype,
			    &flags));
			if (received < 0) {
				const int recv_errno = errno;
				mark_failed(session, execution, recv_errno == EWOULDBLOCK || recv_errno == EAGAIN
				    ? "timed out waiting for trigger message"
				    : strerror_string(recv_errno));
				return;
			}
			if ((flags & MSG_NOTIFICATION) != 0)
				continue;
			buffer[received] = '\0';
			if (!trigger_payload.empty() && trigger_payload != buffer) {
				mark_failed(session, execution, "unexpected trigger payload");
				return;
			}
			break;
		}
		for (const MessageSpec& message : server_messages) {
			struct sctp_sndinfo sndinfo {};
			struct iovec send_iov {};
			send_iov.iov_base = const_cast<char*>(message.payload.data());
			send_iov.iov_len = message.payload.size();
			sndinfo.snd_sid = message.stream;
			sndinfo.snd_ppid = message.ppid;
			sndinfo.snd_assoc_id = rcvinfo.rcv_assoc_id;
			if (sctp_sendv(fd,
			        &send_iov,
			        1,
			        nullptr,
			        0,
			        &sndinfo,
			        static_cast<socklen_t>(sizeof(sndinfo)),
			        SCTP_SENDV_SNDINFO,
			        0) < 0) {
				const int send_errno = errno;
				mark_failed(session, execution, strerror_string(send_errno));
				return;
			}
		}
		mark_network_complete(session, execution);
	}).detach();
}

[[nodiscard]] FeatureDefinition
make_agent_feature(
    const std::string& id,
    const std::string& title,
    const std::string& category,
    const std::string& summary,
    int timeout_seconds,
    const std::string& instructions_text,
    const std::string& report_prompt,
    const std::vector<std::string>& socket_options = {},
    const std::string& negative_target = {})
{
	FeatureDefinition feature;
	feature.id = id;
	feature.title = title;
	feature.category = category;
	feature.summary = summary;
	feature.instructions_text = instructions_text;
	feature.completion_mode = CompletionMode::AgentReported;
	feature.scenario_kind = ScenarioKind::AgentOnly;
	feature.timeout_seconds = timeout_seconds;
	feature.client_socket_options = socket_options;
	feature.report_prompt = report_prompt;
	feature.negative_connect_target = negative_target;
	return feature;
}

[[nodiscard]] FeatureDefinition
make_receive_feature(
    const std::string& id,
    const std::string& title,
    const std::string& category,
    const std::string& summary,
    CompletionMode completion_mode,
    int timeout_seconds,
    const std::string& instructions_text,
    const std::vector<MessageSpec>& messages,
    const std::vector<std::string>& socket_options = {},
    const std::vector<std::string>& subscriptions = {},
    const std::string& report_prompt = {},
    size_t bind_address_count = 1,
    size_t expected_peer_addr_count = 0)
{
	FeatureDefinition feature;
	feature.id = id;
	feature.title = title;
	feature.category = category;
	feature.summary = summary;
	feature.instructions_text = instructions_text;
	feature.completion_mode = completion_mode;
	feature.scenario_kind = ScenarioKind::ReceiveMessages;
	feature.timeout_seconds = timeout_seconds;
	feature.bind_address_count = bind_address_count;
	feature.client_socket_options = socket_options;
	feature.client_subscriptions = subscriptions;
	feature.client_send_messages = messages;
	feature.report_prompt = report_prompt;
	feature.expected_peer_addr_count = expected_peer_addr_count;
	return feature;
}

[[nodiscard]] FeatureDefinition
make_send_feature(
    const std::string& id,
    const std::string& title,
    const std::string& category,
    const std::string& summary,
    CompletionMode completion_mode,
    int timeout_seconds,
    const std::string& instructions_text,
    const std::string& trigger_payload,
    const std::vector<MessageSpec>& server_messages,
    const std::vector<std::string>& subscriptions,
    const std::string& report_prompt)
{
	FeatureDefinition feature;
	feature.id = id;
	feature.title = title;
	feature.category = category;
	feature.summary = summary;
	feature.instructions_text = instructions_text;
	feature.completion_mode = completion_mode;
	feature.scenario_kind = ScenarioKind::SendAfterTrigger;
	feature.timeout_seconds = timeout_seconds;
	feature.client_subscriptions = subscriptions;
	feature.trigger_payload = trigger_payload;
	feature.server_send_messages = server_messages;
	feature.report_prompt = report_prompt;
	return feature;
}

[[nodiscard]] std::vector<FeatureDefinition>
build_feature_catalog()
{
	std::vector<FeatureDefinition> features;
	features.push_back(make_agent_feature(
	    "socket_create",
	    "Create SCTP socket",
	    "endpoint",
	    "Verify the client environment can create an SCTP socket.",
	    20,
	    "Create an SCTP socket in the client environment and report whether it succeeds.",
	    "Report whether socket(AF_INET, SOCK_SEQPACKET, IPPROTO_SCTP) or the environment equivalent succeeded."));
	features.push_back(make_receive_feature(
	    "bind_listen_connect",
	    "Bind, listen, and connect",
	    "association",
	    "Establish a basic SCTP association against the reference server.",
	    CompletionMode::ServerObserved,
	    20,
	    "Connect to the server and send the exact probe payload once the association is up.",
	    {MessageSpec{"bind-listen-connect", 0, 1}}));
	features.push_back(make_receive_feature(
	    "single_message_boundary",
	    "Single message boundary",
	    "messaging",
	    "Send one SCTP message and preserve its boundary.",
	    CompletionMode::ServerObserved,
	    20,
	    "Connect to the server and send exactly one SCTP message with the specified payload.",
	    {MessageSpec{"single-boundary", 1, 11}}));
	features.push_back(make_receive_feature(
	    "multi_message_boundary",
	    "Multiple message boundaries",
	    "messaging",
	    "Send two SCTP messages and preserve ordering and boundaries.",
	    CompletionMode::ServerObserved,
	    20,
	    "Connect to the server and send the two messages in order as distinct SCTP messages.",
	    {MessageSpec{"alpha", 1, 100}, MessageSpec{"beta", 1, 101}}));
	features.push_back(make_receive_feature(
	    "stream_id",
	    "Stream identifier metadata",
	    "metadata",
	    "Deliver a message with a specific SCTP stream id.",
	    CompletionMode::ServerObserved,
	    20,
	    "Connect to the server and send the message on the requested SCTP stream.",
	    {MessageSpec{"stream-check", 7, 21}}));
	features.push_back(make_receive_feature(
	    "ppid",
	    "PPID metadata",
	    "metadata",
	    "Deliver a message with a specific SCTP PPID.",
	    CompletionMode::ServerObserved,
	    20,
	    "Connect to the server and send the message with the requested SCTP PPID.",
	    {MessageSpec{"ppid-check", 2, 424242}}));
	features.push_back(make_receive_feature(
	    "nodelay",
	    "SCTP_NODELAY",
	    "socket_option",
	    "Set SCTP_NODELAY before sending data.",
	    CompletionMode::Hybrid,
	    20,
	    "Set SCTP_NODELAY on the client socket, connect to the server, send the probe payload, then report the local API result.",
	    {MessageSpec{"nodelay-check", 3, 31}},
	    {"SCTP_NODELAY"},
	    {},
	    "Report whether setting SCTP_NODELAY succeeded in the client environment."));
	features.push_back(make_receive_feature(
	    "initmsg",
	    "SCTP_INITMSG",
	    "socket_option",
	    "Configure SCTP_INITMSG before association setup.",
	    CompletionMode::Hybrid,
	    20,
	    "Configure SCTP_INITMSG or the environment equivalent before connecting, send the probe payload, then report the local API result.",
	    {MessageSpec{"initmsg-check", 4, 41}},
	    {"SCTP_INITMSG"},
	    {},
	    "Report the INITMSG values you attempted and whether the call succeeded."));
	features.push_back(make_receive_feature(
	    "rto_assoc_parameters",
	    "SCTP_RTOINFO",
	    "socket_option",
	    "Configure association RTO parameters before or after association setup, depending on the client API surface.",
	    CompletionMode::Hybrid,
	    20,
	    "Configure SCTP_RTOINFO or the environment equivalent, connect to the server, send the probe payload, then report the RTO values you applied.",
	    {MessageSpec{"rtoinfo-check", 4, 42}},
	    {"SCTP_RTOINFO"},
	    {},
	    "Report the RTO parameter values attempted and whether the API accepted them."));
	features.push_back(make_send_feature(
	    "notifications",
	    "Association and shutdown notifications",
	    "events",
	    "Subscribe to SCTP notifications and report what the client observed.",
	    CompletionMode::Hybrid,
	    20,
	    "Subscribe to association, shutdown, and data I/O notifications. Connect to the server, send the trigger payload, read the server message, close gracefully, then report the notifications you observed.",
	    "notifications-ready",
	    {MessageSpec{"server-notify", 5, 51}},
	    {"association", "shutdown", "dataio"},
	    "Report the notification types observed by the client, including association and shutdown events."));
	features.push_back(make_send_feature(
	    "event_subscription_matrix",
	    "Event subscription matrix",
	    "events",
	    "Subscribe to the SCTP event set exposed by the client environment and report which notifications are surfaced.",
	    CompletionMode::Hybrid,
	    20,
	    "Subscribe to the available SCTP events, connect to the server, send the trigger payload, receive the server message, and report the exact notifications observed.",
	    "event-matrix-ready",
	    {MessageSpec{"server-event-matrix", 5, 52}},
	    {"association", "shutdown", "dataio"},
	    "Report which SCTP notifications were available and which were delivered during the scenario."));
	features.push_back(make_send_feature(
	    "association_shutdown_notifications",
	    "Association shutdown notifications",
	    "events",
	    "Observe graceful association teardown notifications.",
	    CompletionMode::Hybrid,
	    20,
	    "Subscribe to association and shutdown events, connect to the server, send the trigger payload, receive the server message, close the association cleanly, and report the shutdown-related notifications observed.",
	    "shutdown-notify-ready",
	    {MessageSpec{"server-shutdown-notify", 5, 53}},
	    {"association", "shutdown"},
	    "Report the association-change and shutdown notifications observed by the client."));
	features.push_back(make_receive_feature(
	    "multi_bind",
	    "Multihome reference server",
	    "multihoming",
	    "Connect to a multihomed SCTP server contract if the environment supports multiple peer addresses.",
	    CompletionMode::Hybrid,
	    25,
	    "If the client environment supports multihome connect, use all advertised addresses. Otherwise declare the feature unsupported with evidence.",
	    {MessageSpec{"multi-bind-check", 6, 61}},
	    {},
	    {},
	    "Report whether the client used all advertised server addresses when creating the association.",
	    2,
	    0));
	features.push_back(make_receive_feature(
	    "local_addr_enum",
	    "Local address enumeration",
	    "multihoming",
	    "Enumerate client local addresses after association setup.",
	    CompletionMode::Hybrid,
	    20,
	    "Connect to the server, send the trigger payload, enumerate the local SCTP addresses in the client environment, then report them.",
	    {MessageSpec{"local-addr-enum", 0, 71}},
	    {},
	    {},
	    "Report the local addresses returned by the client environment."));
	features.push_back(make_receive_feature(
	    "peer_addr_enum",
	    "Peer address enumeration",
	    "multihoming",
	    "Enumerate peer SCTP addresses after association setup.",
	    CompletionMode::Hybrid,
	    20,
	    "Connect to the server, send the trigger payload, enumerate the peer SCTP addresses in the client environment, then report them.",
	    {MessageSpec{"peer-addr-enum", 0, 72}},
	    {},
	    {},
	    "Report the peer addresses returned by the client environment."));
	features.push_back(make_agent_feature(
	    "negative_connect_error",
	    "Negative connect path",
	    "error_path",
	    "Attempt an invalid SCTP connection and report the failure.",
	    20,
	    "Attempt to connect to the provided invalid target and report the connect or first-send failure.",
	    "Report the error surfaced by the client environment for the invalid SCTP target.",
	    {},
	    "0.0.0.0:1"));
	features.push_back(make_receive_feature(
	    "default_sndinfo_recvrcvinfo",
	    "SCTP_DEFAULT_SNDINFO / RECVRCVINFO",
	    "metadata",
	    "Set default sndinfo and confirm the server receives the expected metadata.",
	    CompletionMode::Hybrid,
	    20,
	    "Set SCTP_DEFAULT_SNDINFO or the environment equivalent, then send the probe payload without overriding stream or PPID per message.",
	    {MessageSpec{"default-sndinfo", 9, 901}},
	    {"SCTP_DEFAULT_SNDINFO"},
	    {},
	    "Report whether setting the default send info succeeded."));
	features.push_back(make_receive_feature(
	    "unordered_delivery",
	    "Unordered delivery",
	    "messaging",
	    "Send an unordered SCTP message if the client environment exposes the necessary flag or sndinfo field.",
	    CompletionMode::Hybrid,
	    20,
	    "Send the probe payload as an unordered SCTP message if supported, then report whether the environment exposed and accepted unordered delivery controls.",
	    {MessageSpec{"unordered-check", 12, 1201}},
	    {},
	    {},
	    "Report whether unordered delivery controls were available and whether the send path accepted them."));
	features.push_back(make_send_feature(
	    "recvnxtinfo",
	    "SCTP_RECVNXTINFO",
	    "metadata",
	    "Receive two server messages and report next-message metadata from the client side.",
	    CompletionMode::Hybrid,
	    20,
	    "Enable SCTP_RECVNXTINFO or the environment equivalent. Connect to the server, send the trigger payload, receive the two server messages, and report the next-message metadata you observed.",
	    "recvnxtinfo-ready",
	    {MessageSpec{"nxt-first", 10, 1001}, MessageSpec{"nxt-second", 11, 1002}},
	    {},
	    "Report the next-message metadata observed by the client while receiving the first message."));
	features.push_back(make_agent_feature(
	    "autoclose",
	    "SCTP_AUTOCLOSE",
	    "socket_option",
	    "Attempt to configure SCTP_AUTOCLOSE on the client socket.",
	    20,
	    "Call SCTP_AUTOCLOSE or the environment equivalent and report whether the option is available and accepted.",
	    "Report the AUTOCLOSE value attempted and the outcome.",
	    {"SCTP_AUTOCLOSE"}));
	features.push_back(make_receive_feature(
	    "bindx_add_remove",
	    "SCTP_BINDX add/remove",
	    "multihoming",
	    "Add and remove local SCTP bind addresses on the client before connecting.",
	    CompletionMode::Hybrid,
	    25,
	    "If the environment exposes SCTP_BINDX or equivalent multihome bind controls, add and remove local addresses as needed, connect to the server, send the probe payload, then report the bindx operations attempted.",
	    {MessageSpec{"bindx-check", 13, 1301}},
	    {},
	    {},
	    "Report the local addresses added or removed and whether the API accepted the operations."));
	features.push_back(make_receive_feature(
	    "primary_addr_management",
	    "Primary address management",
	    "multihoming",
	    "Attempt to set a local primary address for the association.",
	    CompletionMode::Hybrid,
	    25,
	    "Connect to the server, attempt to set the local primary address if the API supports it, send the probe payload, and report the result.",
	    {MessageSpec{"primary-addr-check", 14, 1401}},
	    {},
	    {},
	    "Report whether the client environment exposed local primary-address management and the result of the call."));
	features.push_back(make_receive_feature(
	    "peer_primary_addr_request",
	    "Peer primary address request",
	    "multihoming",
	    "Attempt to request a peer primary address change for the association.",
	    CompletionMode::Hybrid,
	    25,
	    "Connect to the server, attempt to request a peer primary address change if the API supports it, send the probe payload, and report the result.",
	    {MessageSpec{"peer-primary-check", 14, 1402}},
	    {},
	    {},
	    "Report whether the client environment exposed peer primary-address requests and the result of the call."));
	features.push_back(make_receive_feature(
	    "peeloff_assoc",
	    "Association peeloff",
	    "association",
	    "Peel the association onto a dedicated socket if the client environment supports peeloff.",
	    CompletionMode::Hybrid,
	    20,
	    "Connect to the server, peel off the association to a dedicated socket if supported, send the probe payload on the peeled-off path, and report the result.",
	    {MessageSpec{"peeloff-check", 15, 1501}},
	    {},
	    {},
	    "Report whether the association peeloff API was available and whether it succeeded."));
	features.push_back(make_receive_feature(
	    "assoc_id_listing",
	    "Association identifier listing",
	    "association",
	    "Enumerate association identifiers after connecting.",
	    CompletionMode::Hybrid,
	    20,
	    "Connect to the server, send the probe payload, enumerate association identifiers if the API supports it, and report the results.",
	    {MessageSpec{"assoc-id-list", 15, 1502}},
	    {},
	    {},
	    "Report the association identifiers returned by the client environment, or explain why enumeration is unavailable."));
	features.push_back(make_receive_feature(
	    "assoc_status_opt_info",
	    "SCTP_STATUS / opt_info",
	    "introspection",
	    "Query association status from the client after connecting.",
	    CompletionMode::Hybrid,
	    20,
	    "Connect to the server, send the probe payload, query association status through sctp_opt_info or an equivalent API, then report the result.",
	    {MessageSpec{"assoc-status", 0, 81}},
	    {},
	    {},
	    "Report whether association status information was available and summarize the returned state."));
	features.push_back(make_send_feature(
	    "stream_reconfig_reset",
	    "Stream reconfiguration reset",
	    "reconfiguration",
	    "Attempt an SCTP stream reset against the active association.",
	    CompletionMode::Hybrid,
	    25,
	    "Connect to the server, send the trigger payload, attempt stream reset or the environment equivalent, and report the outcome.",
	    "stream-reset-ready",
	    {MessageSpec{"stream-reset-ack", 16, 1601}},
	    {},
	    "Report which stream reset operation was attempted and whether the client environment accepted it."));
	features.push_back(make_send_feature(
	    "stream_reconfig_add_streams",
	    "Stream reconfiguration add streams",
	    "reconfiguration",
	    "Attempt to add outbound or inbound streams on the active association.",
	    CompletionMode::Hybrid,
	    25,
	    "Connect to the server, send the trigger payload, attempt to add streams through the environment's SCTP reconfiguration API, and report the outcome.",
	    "stream-add-ready",
	    {MessageSpec{"stream-add-ack", 16, 1602}},
	    {},
	    "Report the requested stream changes and whether the client environment accepted them."));
	return features;
}

class FeatureServer {
public:
	explicit FeatureServer(ServerOptions options)
	    : options_(std::move(options))
	    , feature_catalog_(build_feature_catalog())
	{
		if (options_.sctp_advertise_addrs.empty())
			options_.sctp_advertise_addrs = options_.sctp_bind_addrs;
		for (const FeatureDefinition& feature : feature_catalog_)
			feature_index_[feature.id] = &feature;
	}

	int Run()
	{
		std::string preflight_error;
		if (!CheckSctpAvailable(preflight_error)) {
			std::cerr << preflight_error << "\n";
			return 1;
		}
		const int listen_fd = CreateHttpSocket();
		if (listen_fd < 0)
			return 1;
		std::cerr << "sctp-feature-server listening on " << options_.http_host << ":" << options_.http_port << "\n";
		std::cerr << "advertising SCTP addresses: " << join(options_.sctp_advertise_addrs, ", ") << "\n";
		while (true) {
			int client_fd = accept(listen_fd, nullptr, nullptr);
			if (client_fd < 0) {
				if (errno == EINTR)
					continue;
				std::perror("accept");
				break;
			}
			std::thread([this, client_fd]() {
				HandleClient(client_fd);
				close(client_fd);
			}).detach();
		}
		close(listen_fd);
		return 1;
	}

private:
	ServerOptions options_;
	std::vector<FeatureDefinition> feature_catalog_;
	std::unordered_map<std::string, const FeatureDefinition*> feature_index_;
	std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
	mutable std::mutex sessions_mutex_;

	bool CheckSctpAvailable(std::string& error)
	{
		int fd = socket(AF_INET, SOCK_SEQPACKET, IPPROTO_SCTP);
		if (fd < 0) {
			error = "SCTP is unavailable on this FreeBSD host. Run `kldload /boot/kernel/sctp.ko` as root and restart the server.";
			return false;
		}
		close(fd);
		return true;
	}

	int CreateHttpSocket() const
	{
		struct addrinfo hints {};
		struct addrinfo* result = nullptr;
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
		const std::string port = std::to_string(options_.http_port);
		const int rc = getaddrinfo(options_.http_host == "0.0.0.0" ? nullptr : options_.http_host.c_str(), port.c_str(), &hints, &result);
		if (rc != 0) {
			std::cerr << "getaddrinfo: " << gai_strerror(rc) << "\n";
			return -1;
		}
		int listen_fd = -1;
		for (struct addrinfo* it = result; it != nullptr; it = it->ai_next) {
			listen_fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
			if (listen_fd < 0)
				continue;
			int on = 1;
			setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
			if (bind(listen_fd, it->ai_addr, it->ai_addrlen) == 0 && listen(listen_fd, kBacklog) == 0)
				break;
			close(listen_fd);
			listen_fd = -1;
		}
		freeaddrinfo(result);
		if (listen_fd < 0)
			std::perror("bind/listen");
		return listen_fd;
	}

	void HandleClient(int client_fd)
	{
		std::string timeout_error;
		if (!set_http_timeout(client_fd, 5, timeout_error)) {
			WriteHttpResponse(client_fd, HttpResponse{500, json_error(timeout_error)});
			return;
		}
		HttpRequest request;
		std::string error;
		if (!ReadHttpRequest(client_fd, request, error)) {
			const int status = error == "timed out reading HTTP request" ? 408 : 400;
			WriteHttpResponse(client_fd, HttpResponse{status, json_error(error)});
			return;
		}
		if (HandleSummaryStream(client_fd, request))
			return;
		WriteHttpResponse(client_fd, HandleRequest(request));
	}

	bool ReadHttpRequest(int client_fd, HttpRequest& request, std::string& error)
	{
		std::string raw;
		std::array<char, 4096> buffer {};
		size_t header_end = std::string::npos;
		while ((header_end = raw.find("\r\n\r\n")) == std::string::npos &&
		    (header_end = raw.find("\n\n")) == std::string::npos) {
			ssize_t received = read(client_fd, buffer.data(), buffer.size());
			if (received <= 0) {
				error = (received < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
				    ? "timed out reading HTTP request"
				    : "failed to read HTTP request";
				return false;
			}
			raw.append(buffer.data(), static_cast<size_t>(received));
			if (raw.size() > 1 << 20) {
				error = "request too large";
				return false;
			}
		}
		const std::string header_block = raw.substr(0, header_end);
		std::istringstream header_stream(header_block);
		std::string request_line;
		if (!std::getline(header_stream, request_line)) {
			error = "missing request line";
			return false;
		}
		if (!request_line.empty() && request_line.back() == '\r')
			request_line.pop_back();
		std::istringstream request_line_stream(request_line);
		std::string http_version;
		if (!(request_line_stream >> request.method >> request.path >> http_version)) {
			error = "invalid request line";
			return false;
		}
		std::string header_line;
		while (std::getline(header_stream, header_line)) {
			if (!header_line.empty() && header_line.back() == '\r')
				header_line.pop_back();
			const size_t colon = header_line.find(':');
			if (colon == std::string::npos)
				continue;
			std::string key = trim(header_line.substr(0, colon));
			std::string value = trim(header_line.substr(colon + 1));
			std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			request.headers[key] = value;
		}
		size_t content_length = 0;
		auto header_it = request.headers.find("content-length");
		if (header_it != request.headers.end())
			content_length = static_cast<size_t>(std::strtoul(header_it->second.c_str(), nullptr, 10));
		const size_t body_offset = raw.compare(header_end, 4, "\r\n\r\n") == 0 ? header_end + 4 : header_end + 2;
		request.body = raw.substr(body_offset);
		while (request.body.size() < content_length) {
			ssize_t received = read(client_fd, buffer.data(), buffer.size());
			if (received <= 0) {
				error = (received < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
				    ? "timed out reading HTTP body"
				    : "failed to read HTTP body";
				return false;
			}
			request.body.append(buffer.data(), static_cast<size_t>(received));
		}
		if (request.body.size() > content_length)
			request.body.resize(content_length);
		return true;
	}

	void WriteHttpResponse(int client_fd, const HttpResponse& response)
	{
		std::ostringstream out;
		out << "HTTP/1.1 " << response.status << " " << http_status_text(response.status) << "\r\n";
		out << "Content-Type: " << response.content_type << "\r\n";
		for (const auto& header : response.headers)
			out << header.first << ": " << header.second << "\r\n";
		out << "Content-Length: " << response.body.size() << "\r\n";
		out << "Connection: close\r\n\r\n";
		out << response.body;
		(void)write_all(client_fd, out.str());
	}

	HttpResponse HandleRequest(const HttpRequest& request)
	{
		std::vector<std::string> parts = split_path(request.path);
		if (request.method == "GET" && request.path == "/")
			return {200, SessionIndexHtml(), "text/html; charset=utf-8"};
		if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "dashboard" && request.method == "GET") {
			std::shared_ptr<Session> session = FindSession(parts[1]);
			if (!session)
				return {404, json_error("unknown session")};
			return {200, DashboardHtml(session), "text/html; charset=utf-8"};
		}
		if (request.method == "GET" && request.path == "/healthz")
			return {200, "{\"ok\":true}"};
		if (request.method == "GET" && request.path == "/v1/features")
			return {200, FeaturesJson()};
		if (request.method == "POST" && request.path == "/v1/sessions")
			return CreateSession(request.body);

		if (parts.size() >= 3 && parts[0] == "v1" && parts[1] == "sessions") {
			std::shared_ptr<Session> session = FindSession(parts[2]);
			if (!session)
				return {404, json_error("unknown session")};
			if (parts.size() == 3 && request.method == "GET")
				return {200, SessionJson(session)};
			if (parts.size() == 4 && parts[3] == "summary" && request.method == "GET")
				return {200, SummaryJson(session)};
			if (parts.size() >= 5 && parts[3] == "features") {
				const FeatureDefinition* feature = FindFeature(parts[4]);
				if (feature == nullptr)
					return {404, json_error("unknown feature")};
				std::shared_ptr<FeatureExecution> execution = session->features[feature->id];
				maybe_timeout(session, execution);
				if (parts.size() == 5 && request.method == "GET")
					return {200, FeatureJson(session, execution)};
				if (parts.size() == 6 && parts[5] == "start" && request.method == "POST")
					return StartFeature(session, execution);
				if (parts.size() == 6 && parts[5] == "complete" && request.method == "POST")
					return CompleteFeature(session, execution, request.body);
				if (parts.size() == 6 && parts[5] == "unsupported" && request.method == "POST")
					return UnsupportedFeature(session, execution, request.body);
			}
		}
		return {404, json_error("unknown route")};
	}

	std::vector<std::string> split_path(const std::string& path) const
	{
		std::vector<std::string> parts;
		std::string current;
		for (char ch : path) {
			if (ch == '?')
				break;
			if (ch == '/') {
				if (!current.empty()) {
					parts.push_back(current);
					current.clear();
				}
			} else {
				current.push_back(ch);
			}
		}
		if (!current.empty())
			parts.push_back(current);
		return parts;
	}

	std::shared_ptr<Session> FindSession(const std::string& id)
	{
		std::lock_guard<std::mutex> lock(sessions_mutex_);
		auto it = sessions_.find(id);
		if (it == sessions_.end())
			return nullptr;
		return it->second;
	}

	const FeatureDefinition* FindFeature(const std::string& id) const
	{
		auto it = feature_index_.find(id);
		return it == feature_index_.end() ? nullptr : it->second;
	}

	std::string FeaturesJson() const
	{
		std::ostringstream out;
		out << "{"
		    << "\"server\":\"sctp-feature-server\","
		    << "\"features\":[";
		for (size_t i = 0; i < feature_catalog_.size(); i++) {
			const FeatureDefinition& feature = feature_catalog_[i];
			if (i != 0)
				out << ",";
			out << "{"
			    << "\"id\":" << json_quote(feature.id) << ","
			    << "\"title\":" << json_quote(feature.title) << ","
			    << "\"category\":" << json_quote(feature.category) << ","
			    << "\"summary\":" << json_quote(feature.summary) << ","
			    << "\"completion_mode\":" << json_quote(to_string(feature.completion_mode)) << ","
			    << "\"timeout_seconds\":" << feature.timeout_seconds
			    << "}";
		}
		out << "]"
		    << "}";
		return out.str();
	}

	HttpResponse CreateSession(const std::string& body)
	{
		std::map<std::string, std::string> params;
		std::string error;
		if (!parse_simple_json_object(body, params, error))
			return {400, json_error(error)};
		auto session = std::make_shared<Session>();
		session->id = RandomId();
		session->agent_name = params["agent_name"];
		session->environment_name = params["environment_name"];
		session->created_at = Clock::now();
		for (const FeatureDefinition& feature : feature_catalog_) {
			auto execution = std::make_shared<FeatureExecution>(&feature);
			session->features.emplace(feature.id, std::move(execution));
		}
		{
			std::lock_guard<std::mutex> lock(sessions_mutex_);
			sessions_[session->id] = session;
		}
		return {201, SessionJson(session)};
	}

	std::string SessionJson(const std::shared_ptr<Session>& session)
	{
		std::lock_guard<std::mutex> lock(session->mutex);
		std::ostringstream out;
		out << "{"
		    << "\"session_id\":" << json_quote(session->id) << ","
		    << "\"agent_name\":" << json_quote(session->agent_name) << ","
		    << "\"environment_name\":" << json_quote(session->environment_name) << ","
		    << "\"created_at\":" << json_quote(iso_time(session->created_at)) << ","
		    << "\"active_feature_id\":" << json_quote(session->active_feature_id) << ","
		    << "\"dashboard_path\":" << json_quote(DashboardPath(session->id)) << ","
		    << "\"features\":[";
		bool first = true;
		for (const FeatureDefinition& feature : feature_catalog_) {
			if (!first)
				out << ",";
			first = false;
			out << FeatureJson(session, session->features.at(feature.id), false);
		}
		out << "]"
		    << "}";
		return out.str();
	}

	std::string SummaryJson(const std::shared_ptr<Session>& session)
	{
		int passed = 0;
		int failed = 0;
		int unsupported = 0;
		int timed_out = 0;
		int pending = 0;
		int active = 0;
		std::ostringstream features;
		features << "[";
		bool first = true;
		for (const FeatureDefinition& feature : feature_catalog_) {
			std::shared_ptr<FeatureExecution> execution = session->features.at(feature.id);
			maybe_timeout(session, execution);
			std::lock_guard<std::mutex> lock(execution->mutex);
			switch (execution->state) {
			case FeatureState::Passed:
				passed++;
				break;
			case FeatureState::Failed:
				failed++;
				break;
			case FeatureState::Unsupported:
				unsupported++;
				break;
			case FeatureState::TimedOut:
				timed_out++;
				break;
			case FeatureState::Pending:
				pending++;
				break;
			case FeatureState::Active:
				active++;
				break;
			}
			if (!first)
				features << ",";
			first = false;
			features << FeatureJsonLocked(execution, false);
		}
		features << "]";
		std::ostringstream out;
		out << "{"
		    << "\"session_id\":" << json_quote(session->id) << ","
		    << "\"passed\":" << passed << ","
		    << "\"failed\":" << failed << ","
		    << "\"unsupported\":" << unsupported << ","
		    << "\"timed_out\":" << timed_out << ","
		    << "\"pending\":" << pending << ","
		    << "\"active\":" << active << ","
		    << "\"pending_or_active\":" << (pending + active) << ","
		    << "\"complete\":" << (pending == 0 && active == 0 ? "true" : "false") << ","
		    << "\"features\":" << features.str()
		    << "}";
		return out.str();
	}

	std::string FeatureJsonLocked(const std::shared_ptr<FeatureExecution>& execution, bool include_contract = true)
	{
		std::ostringstream out;
		out << "{"
		    << "\"id\":" << json_quote(execution->definition->id) << ","
		    << "\"title\":" << json_quote(execution->definition->title) << ","
		    << "\"category\":" << json_quote(execution->definition->category) << ","
		    << "\"completion_mode\":" << json_quote(to_string(execution->definition->completion_mode)) << ","
		    << "\"state\":" << json_quote(to_string(execution->state)) << ","
		    << "\"message\":" << json_quote(execution->message) << ","
		    << "\"started_at\":" << json_quote(iso_time(execution->started_at)) << ","
		    << "\"deadline_at\":" << json_quote(iso_time(execution->deadline_at)) << ","
		    << "\"finished_at\":" << json_quote(iso_time(execution->finished_at)) << ","
		    << "\"evidence_kind\":" << json_quote(execution->evidence_kind) << ","
		    << "\"evidence_text\":" << json_quote(execution->evidence_text) << ","
		    << "\"report_text\":" << json_quote(execution->report_text);
		if (include_contract && !execution->contract_json.empty())
			out << ",\"contract\":" << execution->contract_json;
		out << "}";
		return out.str();
	}

	std::string FeatureJson(const std::shared_ptr<Session>& session, const std::shared_ptr<FeatureExecution>& execution, bool include_contract = true)
	{
		(void)session;
		std::lock_guard<std::mutex> lock(execution->mutex);
		return FeatureJsonLocked(execution, include_contract);
	}

	std::string DashboardPath(const std::string& session_id) const
	{
		return "/sessions/" + session_id + "/dashboard";
	}

	std::string SessionApiPath(const std::string& session_id) const
	{
		return "/v1/sessions/" + session_id;
	}

	std::string SummaryPath(const std::string& session_id) const
	{
		return SessionApiPath(session_id) + "/summary";
	}

	std::string SummaryStreamPath(const std::string& session_id) const
	{
		return SummaryPath(session_id) + "/stream";
	}

	bool HandleSummaryStream(int client_fd, const HttpRequest& request)
	{
		if (request.method != "GET")
			return false;
		std::vector<std::string> parts = split_path(request.path);
		if (parts.size() != 5 || parts[0] != "v1" || parts[1] != "sessions" || parts[3] != "summary" || parts[4] != "stream")
			return false;

		std::shared_ptr<Session> session = FindSession(parts[2]);
		if (!session) {
			WriteHttpResponse(client_fd, HttpResponse{404, json_error("unknown session")});
			return true;
		}

		std::ostringstream headers;
		headers << "HTTP/1.1 200 " << http_status_text(200) << "\r\n";
		headers << "Content-Type: text/event-stream; charset=utf-8\r\n";
		headers << "Cache-Control: no-cache\r\n";
		headers << "Connection: keep-alive\r\n";
		headers << "X-Accel-Buffering: no\r\n\r\n";
		if (!write_all(client_fd, headers.str()))
			return true;
		if (!write_all(client_fd, "retry: 1000\n\n"))
			return true;

		std::string last_summary;
		auto last_heartbeat = std::chrono::steady_clock::now();
		while (true) {
			std::string summary = SummaryJson(session);
			if (summary != last_summary) {
				if (!WriteSseEvent(client_fd, "summary", summary))
					return true;
				last_summary = std::move(summary);
				last_heartbeat = std::chrono::steady_clock::now();
			} else {
				auto now = std::chrono::steady_clock::now();
				if (now - last_heartbeat >= std::chrono::seconds(2)) {
					if (!write_all(client_fd, ": keep-alive\n\n"))
						return true;
					last_heartbeat = now;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
	}

	bool WriteSseEvent(int client_fd, const std::string& event_name, const std::string& data)
	{
		std::ostringstream out;
		if (!event_name.empty())
			out << "event: " << event_name << "\n";
		if (data.empty()) {
			out << "data:\n\n";
			return write_all(client_fd, out.str());
		}
		std::istringstream input(data);
		std::string line;
		while (std::getline(input, line))
			out << "data: " << line << "\n";
		out << "\n";
		return write_all(client_fd, out.str());
	}

	std::string SessionIndexHtml() const
	{
		struct SessionCard {
			std::string id;
			std::string agent_name;
			std::string environment_name;
			std::string created_at;
			std::string active_feature_id;
			std::string dashboard_path;
			Clock::time_point created_at_point {};
		};

		std::vector<SessionCard> active_sessions;
		std::vector<SessionCard> other_sessions;
		{
			std::lock_guard<std::mutex> lock(sessions_mutex_);
			for (const auto& item : sessions_) {
				const std::shared_ptr<Session>& session = item.second;
				SessionCard card;
				{
					std::lock_guard<std::mutex> session_lock(session->mutex);
					card.id = session->id;
					card.agent_name = session->agent_name;
					card.environment_name = session->environment_name;
					card.created_at = iso_time(session->created_at);
					card.created_at_point = session->created_at;
					card.active_feature_id = session->active_feature_id;
					card.dashboard_path = DashboardPath(session->id);
				}
				if (!card.active_feature_id.empty())
					active_sessions.push_back(std::move(card));
				else
					other_sessions.push_back(std::move(card));
			}
		}

		auto newest_first = [](const SessionCard& a, const SessionCard& b) {
			return a.created_at_point > b.created_at_point;
		};
		std::sort(active_sessions.begin(), active_sessions.end(), newest_first);
		std::sort(other_sessions.begin(), other_sessions.end(), newest_first);

		auto render_cards = [&](const std::vector<SessionCard>& cards, const std::string& empty_text) {
			std::ostringstream section;
			if (cards.empty()) {
				section << "<div class=\"empty-state\">" << html_escape(empty_text) << "</div>";
				return section.str();
			}
			section << "<div class=\"session-grid\">";
			for (const SessionCard& card : cards) {
				const bool active = !card.active_feature_id.empty();
				section << "<article class=\"session-card\" data-state=\"" << (active ? "active" : "idle") << "\">"
				        << "<div class=\"session-card-top\">"
				        << "<div>"
				        << "<h2 class=\"session-title\">" << html_escape(card.id) << "</h2>"
				        << "<div class=\"session-meta\">"
				        << "<span>" << html_escape(card.agent_name.empty() ? "unknown-agent" : card.agent_name) << "</span>"
				        << "<span>" << html_escape(card.environment_name.empty() ? "unknown-environment" : card.environment_name) << "</span>"
				        << "</div>"
				        << "</div>"
				        << "<div class=\"session-pill\" data-state=\"" << (active ? "active" : "idle") << "\">"
				        << (active ? "active" : "idle")
				        << "</div>"
				        << "</div>"
				        << "<div class=\"session-body\">"
				        << "<div class=\"row\"><span class=\"label\">Created</span><span class=\"value\">"
				        << html_escape(card.created_at.empty() ? "unknown" : card.created_at)
				        << "</span></div>"
				        << "<div class=\"row\"><span class=\"label\">Feature</span><span class=\"value\">"
				        << html_escape(active ? card.active_feature_id : "none")
				        << "</span></div>"
				        << "</div>"
				        << "<a class=\"session-link\" href=\"" << html_escape(card.dashboard_path) << "\">Open dashboard</a>"
				        << "</article>";
			}
			section << "</div>";
			return section.str();
		};

		std::ostringstream out;
		out << R"__HTML__(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta http-equiv="refresh" content="3">
  <title>SCTP Session Index</title>
  <style>
    :root {
      --bg: #f4efe6;
      --panel: rgba(255, 252, 247, 0.92);
      --panel-strong: #fffaf2;
      --ink: #182126;
      --muted: #59636c;
      --border: rgba(24, 33, 38, 0.12);
      --shadow: 0 28px 70px rgba(40, 31, 12, 0.12);
      --green: #2b7a4b;
      --amber: #b87912;
      --gray: #74818a;
    }

    * { box-sizing: border-box; }

    body {
      margin: 0;
      min-height: 100vh;
      font-family: "Iowan Old Style", "Palatino Linotype", "Book Antiqua", Palatino, Georgia, serif;
      color: var(--ink);
      background:
        radial-gradient(circle at top left, rgba(184, 121, 18, 0.16), transparent 32%),
        radial-gradient(circle at top right, rgba(43, 122, 75, 0.12), transparent 28%),
        linear-gradient(180deg, #fcf7ef 0%, var(--bg) 55%, #ebe4d8 100%);
    }

    .shell {
      width: min(1180px, calc(100vw - 32px));
      margin: 24px auto 40px;
    }

    .hero,
    .section,
    .session-card {
      background: var(--panel);
      border: 1px solid var(--border);
      box-shadow: var(--shadow);
      backdrop-filter: blur(10px);
    }

    .hero,
    .section {
      padding: 24px;
      border-radius: 24px;
    }

    .section {
      margin-top: 18px;
    }

    h1,
    h2,
    .subtitle,
    .session-meta,
    .session-body,
    .refresh-note,
    .empty-state {
      margin: 0;
    }

    h1 {
      font-size: clamp(2rem, 4.8vw, 3.2rem);
      line-height: 0.96;
      letter-spacing: -0.04em;
    }

    .subtitle,
    .refresh-note,
    .session-meta,
    .session-body,
    .empty-state {
      font-family: "SFMono-Regular", Menlo, Consolas, "Liberation Mono", monospace;
    }

    .subtitle,
    .refresh-note {
      color: var(--muted);
      font-size: 0.92rem;
    }

    .subtitle {
      margin-top: 10px;
    }

    .refresh-note {
      margin-top: 16px;
    }

    .hero-stats {
      display: grid;
      gap: 16px;
      grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
      margin-top: 20px;
    }

    .stat {
      padding: 16px 18px;
      border-radius: 18px;
      background: var(--panel-strong);
      border: 1px solid var(--border);
    }

    .stat .label {
      color: var(--muted);
      font-size: 0.8rem;
      text-transform: uppercase;
      letter-spacing: 0.12em;
    }

    .stat .value {
      margin-top: 8px;
      font-size: 2rem;
      line-height: 1;
    }

    .section-title {
      font-size: 1.3rem;
      line-height: 1.1;
    }

    .section-copy {
      margin-top: 8px;
      color: var(--muted);
      font-size: 0.92rem;
    }

    .session-grid {
      margin-top: 18px;
      display: grid;
      gap: 16px;
      grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
    }

    .session-card {
      border-radius: 20px;
      padding: 18px;
      position: relative;
      overflow: hidden;
    }

    .session-card::before {
      content: "";
      position: absolute;
      inset: 0 auto 0 0;
      width: 8px;
      background: var(--gray);
    }

    .session-card[data-state="active"]::before {
      background: var(--amber);
    }

    .session-card-top {
      display: flex;
      gap: 12px;
      align-items: flex-start;
      justify-content: space-between;
    }

    .session-title {
      font-size: 1.12rem;
      line-height: 1.18;
      overflow-wrap: anywhere;
    }

    .session-meta {
      margin-top: 10px;
      color: var(--muted);
      font-size: 0.76rem;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      display: flex;
      gap: 10px;
      flex-wrap: wrap;
    }

    .session-pill {
      flex: 0 0 auto;
      padding: 7px 11px;
      border-radius: 999px;
      border: 1px solid currentColor;
      font-size: 0.72rem;
      text-transform: uppercase;
      letter-spacing: 0.12em;
      background: rgba(255, 255, 255, 0.8);
      color: var(--gray);
    }

    .session-pill[data-state="active"] {
      color: var(--amber);
    }

    .session-body {
      margin-top: 16px;
      color: var(--muted);
      font-size: 0.82rem;
      line-height: 1.55;
    }

    .row {
      display: flex;
      gap: 12px;
      justify-content: space-between;
      align-items: baseline;
    }

    .row + .row {
      margin-top: 8px;
    }

    .row .label {
      text-transform: uppercase;
      letter-spacing: 0.08em;
    }

    .row .value {
      color: var(--ink);
      text-align: right;
      overflow-wrap: anywhere;
    }

    .session-link {
      display: inline-block;
      margin-top: 18px;
      color: var(--ink);
      text-decoration: none;
      border-bottom: 1px solid rgba(24, 33, 38, 0.24);
      padding-bottom: 2px;
    }

    .empty-state {
      margin-top: 18px;
      padding: 20px;
      border-radius: 18px;
      border: 1px dashed var(--border);
      color: var(--muted);
      text-align: center;
    }

    @media (max-width: 720px) {
      .shell { width: min(100vw - 20px, 1180px); margin-top: 10px; }
      .hero, .section { padding: 18px; border-radius: 20px; }
    }
  </style>
</head>
<body>
  <main class="shell">
    <section class="hero">
      <h1>SCTP session index</h1>
      <p class="subtitle">Live dashboard links for the FreeBSD reference server. This page refreshes every 3 seconds.</p>
      <div class="hero-stats">
        <section class="stat">
          <div class="label">Active Sessions</div>
          <div class="value">)__HTML__"
		    << active_sessions.size()
		    << R"__HTML__(</div>
        </section>
        <section class="stat">
          <div class="label">Known Sessions</div>
          <div class="value">)__HTML__"
		    << (active_sessions.size() + other_sessions.size())
		    << R"__HTML__(</div>
        </section>
      </div>
      <p class="refresh-note">Open a dashboard link to watch a single session update in realtime.</p>
    </section>

    <section class="section">
      <h2 class="section-title">Active Sessions</h2>
      <p class="section-copy">Sessions with an in-flight feature are listed here first.</p>
      )__HTML__"
		    << render_cards(active_sessions, "No sessions are active right now.")
		    << R"__HTML__(
    </section>

    <section class="section">
      <h2 class="section-title">Other Sessions</h2>
      <p class="section-copy">Idle or completed sessions stay available until the server restarts.</p>
      )__HTML__"
		    << render_cards(other_sessions, "No additional sessions are currently stored in memory.")
		    << R"__HTML__(
    </section>
  </main>
</body>
</html>)__HTML__";
		return out.str();
	}

	std::string DashboardHtml(const std::shared_ptr<Session>& session) const
	{
		const std::string session_id = session->id;
		const std::string session_path = SessionApiPath(session_id);
		const std::string summary_path = SummaryPath(session_id);
		const std::string stream_path = SummaryStreamPath(session_id);
		std::ostringstream out;
		out << R"__HTML__(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SCTP Traffic Light Dashboard</title>
  <style>
    :root {
      --bg: #f4efe6;
      --panel: rgba(255, 252, 247, 0.92);
      --panel-strong: #fffaf2;
      --ink: #182126;
      --muted: #59636c;
      --border: rgba(24, 33, 38, 0.12);
      --shadow: 0 28px 70px rgba(40, 31, 12, 0.12);
      --green: #2b7a4b;
      --amber: #b87912;
      --red: #b03a2e;
      --gray: #74818a;
      --live: #14532d;
      --reconnecting: #8a5a11;
      --error: #8b1e16;
    }

    * { box-sizing: border-box; }

    body {
      margin: 0;
      min-height: 100vh;
      font-family: "Iowan Old Style", "Palatino Linotype", "Book Antiqua", Palatino, Georgia, serif;
      color: var(--ink);
      background:
        radial-gradient(circle at top left, rgba(184, 121, 18, 0.16), transparent 32%),
        radial-gradient(circle at top right, rgba(43, 122, 75, 0.12), transparent 28%),
        linear-gradient(180deg, #fcf7ef 0%, var(--bg) 55%, #ebe4d8 100%);
    }

    .shell {
      width: min(1180px, calc(100vw - 32px));
      margin: 24px auto 40px;
    }

    .hero,
    .panel,
    .feature-card {
      background: var(--panel);
      border: 1px solid var(--border);
      box-shadow: var(--shadow);
      backdrop-filter: blur(10px);
    }

    .hero {
      padding: 24px;
      border-radius: 24px;
    }

    .hero-top {
      display: flex;
      gap: 16px;
      align-items: flex-start;
      justify-content: space-between;
      flex-wrap: wrap;
    }

    h1 {
      margin: 0;
      font-size: clamp(2rem, 5vw, 3.4rem);
      line-height: 0.94;
      letter-spacing: -0.04em;
    }

    .subtitle,
    .note,
    .feature-meta,
    .feature-time,
    .empty-state,
    .error {
      font-family: "SFMono-Regular", Menlo, Consolas, "Liberation Mono", monospace;
    }

    .subtitle {
      margin-top: 10px;
      color: var(--muted);
      font-size: 0.94rem;
    }

    .connection {
      padding: 10px 14px;
      border-radius: 999px;
      border: 1px solid currentColor;
      font-size: 0.8rem;
      text-transform: uppercase;
      letter-spacing: 0.12em;
      background: rgba(255, 255, 255, 0.75);
    }

    .connection[data-state="live"] { color: var(--live); }
    .connection[data-state="reconnecting"] { color: var(--reconnecting); }
    .connection[data-state="error"] { color: var(--error); }

    .meta-grid,
    .count-grid,
    .feature-grid {
      display: grid;
      gap: 16px;
    }

    .meta-grid,
    .count-grid {
      margin-top: 18px;
      grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
    }

    .panel {
      padding: 16px 18px;
      border-radius: 18px;
    }

    .label {
      color: var(--muted);
      font-size: 0.82rem;
      text-transform: uppercase;
      letter-spacing: 0.12em;
    }

    .value {
      margin-top: 8px;
      font-size: 1.16rem;
      line-height: 1.3;
      overflow-wrap: anywhere;
    }

    .count-grid {
      grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
    }

    .count {
      position: relative;
      overflow: hidden;
      background: var(--panel-strong);
    }

    .count::before {
      content: "";
      position: absolute;
      inset: 0 auto 0 0;
      width: 6px;
      background: var(--gray);
    }

    .count[data-tone="green"]::before { background: var(--green); }
    .count[data-tone="amber"]::before { background: var(--amber); }
    .count[data-tone="red"]::before { background: var(--red); }
    .count[data-tone="gray"]::before { background: var(--gray); }

    .count-number {
      margin-top: 6px;
      font-size: 2rem;
      font-weight: 700;
    }

    .feature-grid {
      margin-top: 22px;
      grid-template-columns: repeat(auto-fit, minmax(360px, 1fr));
    }

    .feature-card {
      position: relative;
      display: flex;
      flex-direction: column;
      border-radius: 20px;
      padding: 18px 18px 16px;
      overflow: hidden;
      min-height: 0;
    }

    .feature-card::before {
      content: "";
      position: absolute;
      inset: 0 auto 0 0;
      width: 8px;
      background: var(--gray);
    }

    .feature-card[data-tone="green"]::before { background: var(--green); }
    .feature-card[data-tone="amber"]::before { background: var(--amber); }
    .feature-card[data-tone="red"]::before { background: var(--red); }
    .feature-card[data-tone="gray"]::before { background: var(--gray); }

    .feature-head {
      display: flex;
      gap: 12px;
      align-items: flex-start;
      justify-content: space-between;
    }

    .feature-title {
      margin: 0;
      font-size: 1.1rem;
      line-height: 1.2;
    }

    .state-pill {
      flex: 0 0 auto;
      padding: 7px 11px;
      border-radius: 999px;
      font-size: 0.72rem;
      text-transform: uppercase;
      letter-spacing: 0.12em;
      border: 1px solid currentColor;
      background: rgba(255, 255, 255, 0.8);
    }

    .state-pill[data-tone="green"] { color: var(--green); }
    .state-pill[data-tone="amber"] { color: var(--amber); }
    .state-pill[data-tone="red"] { color: var(--red); }
    .state-pill[data-tone="gray"] { color: var(--gray); }

    .feature-meta {
      margin-top: 10px;
      color: var(--muted);
      font-size: 0.78rem;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      line-height: 1.5;
    }

    .feature-message {
      margin-top: 14px;
      font-size: 0.98rem;
      line-height: 1.45;
      max-width: 60ch;
    }

    .feature-time {
      margin-top: auto;
      padding-top: 16px;
      border-top: 1px solid rgba(24, 33, 38, 0.08);
      color: var(--muted);
      font-size: 0.78rem;
      line-height: 1.5;
      display: grid;
      gap: 4px;
    }

    .note {
      margin-top: 14px;
      color: var(--muted);
      font-size: 0.82rem;
    }

    .error {
      margin-top: 18px;
      padding: 14px 16px;
      border-radius: 16px;
      border: 1px solid rgba(176, 58, 46, 0.24);
      background: rgba(176, 58, 46, 0.08);
      color: var(--error);
      display: none;
    }

    .empty-state {
      margin-top: 20px;
      padding: 20px;
      border-radius: 18px;
      border: 1px dashed var(--border);
      color: var(--muted);
      text-align: center;
    }

    @media (max-width: 900px) {
      .feature-grid { grid-template-columns: 1fr; }
    }

    @media (max-width: 720px) {
      .shell { width: min(100vw - 20px, 1180px); margin-top: 10px; }
      .hero { padding: 18px; border-radius: 20px; }
      .feature-card { padding: 16px 16px 14px; }
    }
  </style>
</head>
<body>
  <main class="shell">
    <section class="hero">
      <div class="hero-top">
        <div>
          <h1 id="title">SCTP session</h1>
          <div class="subtitle" id="subtitle">Loading session metadata...</div>
        </div>
        <div class="connection" id="connection" data-state="reconnecting">loading</div>
      </div>

      <div class="meta-grid">
        <section class="panel">
          <div class="label">Session</div>
          <div class="value" id="session-id">loading</div>
        </section>
        <section class="panel">
          <div class="label">Agent</div>
          <div class="value" id="agent-name">loading</div>
        </section>
        <section class="panel">
          <div class="label">Environment</div>
          <div class="value" id="environment-name">loading</div>
        </section>
        <section class="panel">
          <div class="label">Active Feature</div>
          <div class="value" id="active-feature">none</div>
        </section>
      </div>

      <div class="count-grid" id="counts"></div>
      <div class="note">Green means passed, amber means active, red means failed or timed out, and gray means pending or unsupported.</div>
      <div class="error" id="error-banner"></div>
    </section>

    <section class="feature-grid" id="feature-grid"></section>
    <div class="empty-state" id="empty-state" hidden>No features are available for this session.</div>
  </main>

  <script>
    const sessionId = )__HTML__";
		out << json_quote(session_id);
		out << R"__HTML__(;
    const sessionPath = )__HTML__";
		out << json_quote(session_path);
		out << R"__HTML__(;
    const summaryPath = )__HTML__";
		out << json_quote(summary_path);
		out << R"__HTML__(;
    const streamPath = )__HTML__";
		out << json_quote(stream_path);
		out << R"__HTML__(;

    const connectionEl = document.getElementById("connection");
    const sessionIdEl = document.getElementById("session-id");
    const subtitleEl = document.getElementById("subtitle");
    const agentEl = document.getElementById("agent-name");
    const environmentEl = document.getElementById("environment-name");
    const activeFeatureEl = document.getElementById("active-feature");
    const countsEl = document.getElementById("counts");
    const featureGridEl = document.getElementById("feature-grid");
    const emptyStateEl = document.getElementById("empty-state");
    const errorBannerEl = document.getElementById("error-banner");

    let stream = null;
    let refreshPromise = null;

    function escapeHtml(value) {
      return String(value ?? "").replace(/[&<>"]/g, (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;" }[ch]));
    }

    function formatTimestamp(value) {
      if (!value) return "n/a";
      const parsed = new Date(value);
      return Number.isNaN(parsed.getTime()) ? value : parsed.toLocaleString();
    }

    function toneForState(state) {
      if (state === "passed") return "green";
      if (state === "active") return "amber";
      if (state === "failed" || state === "timed_out") return "red";
      return "gray";
    }

    function setConnection(state, label) {
      connectionEl.dataset.state = state;
      connectionEl.textContent = label;
    }

    function showError(message) {
      errorBannerEl.textContent = message;
      errorBannerEl.style.display = "block";
    }

    function clearError() {
      errorBannerEl.textContent = "";
      errorBannerEl.style.display = "none";
    }

    async function fetchJson(path) {
      const response = await fetch(path, { cache: "no-store" });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(text || `${response.status} ${response.statusText}`);
      }
      return response.json();
    }

    function renderCounts(summary) {
      const items = [
        ["Passed", summary.passed, "green"],
        ["Active", summary.active, "amber"],
        ["Failed", summary.failed + summary.timed_out, "red"],
        ["Pending", summary.pending, "gray"],
        ["Unsupported", summary.unsupported, "gray"],
      ];
      countsEl.innerHTML = items.map(([label, value, tone]) => `
        <section class="panel count" data-tone="${tone}">
          <div class="label">${escapeHtml(label)}</div>
          <div class="count-number">${escapeHtml(value)}</div>
        </section>
      `).join("");
    }

    function renderFeatures(summary) {
      const features = Array.isArray(summary.features) ? summary.features : [];
      emptyStateEl.hidden = features.length !== 0;
      featureGridEl.innerHTML = features.map((feature) => {
        const tone = toneForState(feature.state);
        return `
          <article class="feature-card" data-tone="${tone}">
            <div class="feature-head">
              <div>
                <h2 class="feature-title">${escapeHtml(feature.title)}</h2>
                <div class="feature-meta">${escapeHtml(feature.category)} / ${escapeHtml(feature.id)}</div>
              </div>
              <div class="state-pill" data-tone="${tone}">${escapeHtml(feature.state)}</div>
            </div>
            <div class="feature-message">${escapeHtml(feature.message || "no status message")}</div>
            <div class="feature-time">
              <div>Started: ${escapeHtml(formatTimestamp(feature.started_at))}</div>
              <div>Finished: ${escapeHtml(formatTimestamp(feature.finished_at))}</div>
            </div>
          </article>
        `;
      }).join("");
    }

    function renderSession(session) {
      document.getElementById("title").textContent = `SCTP session ${session.session_id}`;
      document.title = `SCTP session ${session.session_id}`;
      subtitleEl.textContent = `Realtime execution status for a single in-memory SCTP feature session.`;
      sessionIdEl.textContent = session.session_id || sessionId;
      agentEl.textContent = session.agent_name || "unlabeled";
      environmentEl.textContent = session.environment_name || "unlabeled";
      activeFeatureEl.textContent = session.active_feature_id || "none";
    }

    function renderSummary(summary) {
      const features = Array.isArray(summary.features) ? summary.features : [];
      const activeFeature = features.find((feature) => feature.state === "active");
      renderCounts(summary);
      renderFeatures(summary);
      activeFeatureEl.textContent = activeFeature ? activeFeature.id : "none";
    }

    async function refreshSnapshot() {
      const [session, summary] = await Promise.all([fetchJson(sessionPath), fetchJson(summaryPath)]);
      renderSession(session);
      renderSummary(summary);
      clearError();
    }

    function scheduleRefresh() {
      if (refreshPromise) return;
      refreshPromise = refreshSnapshot()
        .catch((error) => {
          showError(`Live refresh is waiting for the server: ${error.message}`);
        })
        .finally(() => {
          refreshPromise = null;
        });
    }

    function connectStream() {
      if (stream) stream.close();
      stream = new EventSource(streamPath);
      stream.onopen = () => setConnection("live", "live");
      stream.addEventListener("summary", (event) => {
        try {
          renderSummary(JSON.parse(event.data));
          clearError();
          setConnection("live", "live");
        } catch (error) {
          showError(`Received an invalid live update: ${error.message}`);
        }
      });
      stream.onerror = () => {
        setConnection("reconnecting", "reconnecting");
        scheduleRefresh();
      };
    }

    window.addEventListener("beforeunload", () => {
      if (stream) stream.close();
    });

    (async () => {
      setConnection("reconnecting", "loading");
      try {
        await refreshSnapshot();
        setConnection("live", "live");
      } catch (error) {
        showError(`Failed to load session state: ${error.message}`);
      }
      connectStream();
    })();
  </script>
</body>
</html>
)__HTML__";
		return out.str();
	}

	HttpResponse StartFeature(const std::shared_ptr<Session>& session, const std::shared_ptr<FeatureExecution>& execution)
	{
		maybe_timeout(session, execution);
		{
			std::lock_guard<std::mutex> session_lock(session->mutex);
			if (!session->active_feature_id.empty())
				return {409, json_error("another feature is already active in this session")};
		}
		{
			std::lock_guard<std::mutex> lock(execution->mutex);
			if (execution->state != FeatureState::Pending)
				return {409, json_error("feature has already been started in this session")};
		}
		std::string contract;
		std::string error;
		if (execution->definition->scenario_kind == ScenarioKind::AgentOnly) {
			activate_execution(session, execution);
			{
				std::lock_guard<std::mutex> session_lock(session->mutex);
				session->active_feature_id = execution->definition->id;
			}
			contract = build_contract(*execution->definition, {});
		} else if (execution->definition->scenario_kind == ScenarioKind::ReceiveMessages) {
			auto socket_info = create_listening_socket(options_, execution->definition->bind_address_count, execution->definition->timeout_seconds, error);
			if (!socket_info)
				return {409, json_error(error)};
			activate_execution(session, execution);
			set_active_fd(execution, socket_info->fd);
			{
				std::lock_guard<std::mutex> session_lock(session->mutex);
				session->active_feature_id = execution->definition->id;
			}
			contract = build_contract(*execution->definition, socket_info->advertise_addrs);
			run_receive_worker(session, execution, socket_info->fd, execution->definition->client_send_messages, execution->definition->expected_peer_addr_count);
		} else {
			auto socket_info = create_listening_socket(options_, execution->definition->bind_address_count, execution->definition->timeout_seconds, error);
			if (!socket_info)
				return {409, json_error(error)};
			activate_execution(session, execution);
			set_active_fd(execution, socket_info->fd);
			{
				std::lock_guard<std::mutex> session_lock(session->mutex);
				session->active_feature_id = execution->definition->id;
			}
			contract = build_contract(*execution->definition, socket_info->advertise_addrs);
			run_send_after_trigger_worker(session, execution, socket_info->fd, execution->definition->trigger_payload, execution->definition->server_send_messages);
		}
		{
			std::lock_guard<std::mutex> lock(execution->mutex);
			execution->contract_json = contract;
		}
		{
			std::lock_guard<std::mutex> lock(execution->mutex);
			if (execution->state == FeatureState::Active && execution->message == "scenario active" &&
			    execution->definition->completion_mode == CompletionMode::AgentReported) {
				execution->message = "waiting for client completion report";
			}
		}
		return {200, FeatureJson(session, execution)};
	}

	void activate_execution(const std::shared_ptr<Session>&, const std::shared_ptr<FeatureExecution>& execution)
	{
		std::lock_guard<std::mutex> lock(execution->mutex);
		execution->state = FeatureState::Active;
		execution->message = execution->definition->completion_mode == CompletionMode::AgentReported
		    ? "waiting for client completion report"
		    : "scenario active";
		execution->started_at = Clock::now();
		execution->deadline_at = execution->started_at + std::chrono::seconds(execution->definition->timeout_seconds);
	}

	std::string build_contract(const FeatureDefinition& feature, const std::vector<std::string>& connect_addrs) const
	{
		std::ostringstream out;
		out << "{"
		    << "\"feature_id\":" << json_quote(feature.id) << ","
		    << "\"completion_mode\":" << json_quote(to_string(feature.completion_mode)) << ","
		    << "\"transport\":\"sctp4\","
		    << "\"connect_addresses\":" << json_array_strings(connect_addrs) << ","
		    << "\"client_socket_options\":" << json_array_strings(feature.client_socket_options) << ","
		    << "\"client_subscriptions\":" << json_array_strings(feature.client_subscriptions) << ","
		    << "\"client_send_messages\":" << json_messages(feature.client_send_messages) << ","
		    << "\"server_send_messages\":" << json_messages(feature.server_send_messages) << ","
		    << "\"trigger_payload\":" << json_quote(feature.trigger_payload) << ","
		    << "\"negative_connect_target\":" << json_quote(feature.negative_connect_target) << ","
		    << "\"timeout_seconds\":" << feature.timeout_seconds << ","
		    << "\"report_prompt\":" << json_quote(feature.report_prompt) << ","
		    << "\"instructions_text\":" << json_quote(feature.instructions_text)
		    << "}";
		return out.str();
	}

	HttpResponse CompleteFeature(const std::shared_ptr<Session>& session, const std::shared_ptr<FeatureExecution>& execution, const std::string& body)
	{
		maybe_timeout(session, execution);
		std::map<std::string, std::string> params;
		std::string error;
		if (!parse_simple_json_object(body, params, error))
			return {400, json_error(error)};
		bool finished = false;
		{
			std::lock_guard<std::mutex> lock(execution->mutex);
			if (execution->state != FeatureState::Active)
				return {409, json_error("feature is not active")};
			if (execution->definition->completion_mode == CompletionMode::ServerObserved)
				return {409, json_error("feature does not accept client completion reports")};
			execution->agent_complete = true;
			execution->evidence_kind = params["evidence_kind"];
			execution->evidence_text = params["evidence_text"];
			execution->report_text = params["report_text"];
			if (execution->definition->completion_mode == CompletionMode::AgentReported || execution->network_complete) {
				execution->state = FeatureState::Passed;
				execution->message = "scenario completed successfully";
				execution->finished_at = Clock::now();
				finished = true;
			} else {
				execution->message = "waiting for server-side SCTP observation";
			}
		}
		if (finished)
			close_active_fd(execution);
		if (finished)
			clear_active_feature(session, execution->definition->id);
		return {200, FeatureJson(session, execution)};
	}

	HttpResponse UnsupportedFeature(const std::shared_ptr<Session>& session, const std::shared_ptr<FeatureExecution>& execution, const std::string& body)
	{
		maybe_timeout(session, execution);
		std::map<std::string, std::string> params;
		std::string error;
		if (!parse_simple_json_object(body, params, error))
			return {400, json_error(error)};
		{
			std::lock_guard<std::mutex> lock(execution->mutex);
			if (execution->state == FeatureState::Passed)
				return {409, json_error("feature already passed and cannot be marked unsupported")};
			if (execution->state != FeatureState::Pending && execution->state != FeatureState::Active)
				return {409, json_error("feature is already in a terminal state")};
			execution->state = FeatureState::Unsupported;
			execution->message = params["reason"];
			execution->evidence_kind = params["evidence_kind"];
			execution->evidence_text = params["evidence_text"];
			execution->finished_at = Clock::now();
		}
		close_active_fd(execution);
		clear_active_feature(session, execution->definition->id);
		return {200, FeatureJson(session, execution)};
	}

	std::string RandomId() const
	{
		static constexpr char kAlphabet[] = "0123456789abcdef";
		std::random_device device;
		std::uniform_int_distribution<int> dist(0, 15);
		std::string out(16, '0');
		for (char& ch : out)
			ch = kAlphabet[dist(device)];
		return out;
	}
};

bool
parse_options(int argc, char** argv, ServerOptions& options, std::string& error)
{
	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if ((arg == "--http-host" || arg == "--http-port" || arg == "--sctp-addrs" || arg == "--advertise-addrs") && i + 1 >= argc) {
			error = "missing value for " + arg;
			return false;
		}
		if (arg == "--http-host") {
			options.http_host = argv[++i];
		} else if (arg == "--http-port") {
			options.http_port = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 10));
		} else if (arg == "--sctp-addrs") {
			options.sctp_bind_addrs = split(argv[++i], ',');
		} else if (arg == "--advertise-addrs") {
			options.sctp_advertise_addrs = split(argv[++i], ',');
		} else if (arg == "--help" || arg == "-h") {
			std::cout
			    << "usage: sctp-feature-server [--http-host HOST] [--http-port PORT]\n"
			    << "                           [--sctp-addrs addr1[,addr2...]]\n"
			    << "                           [--advertise-addrs addr1[,addr2...]]\n";
			std::exit(0);
		} else {
			error = "unknown option " + arg;
			return false;
		}
	}
	if (options.sctp_bind_addrs.empty()) {
		error = "--sctp-addrs must contain at least one IPv4 address";
		return false;
	}
	if (options.sctp_advertise_addrs.empty())
		options.sctp_advertise_addrs = options.sctp_bind_addrs;
	return true;
}

} // namespace

int
main(int argc, char** argv)
{
	ServerOptions options;
	std::string error;
	if (!parse_options(argc, argv, options, error)) {
		std::cerr << error << "\n";
		return 1;
	}
	FeatureServer server(std::move(options));
	return server.Run();
}
