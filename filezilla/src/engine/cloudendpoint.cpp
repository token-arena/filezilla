#include "cloudendpoint.h"

#include <cstdlib>

namespace cloud {

namespace {

std::string to_lower(std::string_view const& in)
{
	std::string ret(in);
	for (auto & c : ret) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return ret;
}

std::string trimmed(std::string_view const& in)
{
	size_t start = 0;
	while (start < in.size() && (in[start] == ' ' || in[start] == '\t')) {
		++start;
	}
	size_t end = in.size();
	while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\t')) {
		--end;
	}
	return std::string(in.substr(start, end - start));
}

bool is_secure_port(unsigned short port)
{
	return port == 443 || port == 8443;
}

/// Splits "host:port" or "[::1]:port" into host and port. Returns false when the
/// port part is not a valid number.
bool split_port(std::string const& in, std::string & host, unsigned short & port)
{
	host = in;
	port = 0;

	if (host.empty()) {
		return true;
	}

	size_t colon = std::string::npos;
	if (host.front() == '[') {
		auto const close = host.find(']');
		if (close != std::string::npos && close + 1 < host.size() && host[close + 1] == ':') {
			colon = close + 1;
		}
	}
	else {
		colon = host.rfind(':');
	}

	if (colon == std::string::npos) {
		return true;
	}

	std::string const port_str = host.substr(colon + 1);
	if (port_str.empty()) {
		host = host.substr(0, colon);
		return true;
	}

	for (char const c : port_str) {
		if (c < '0' || c > '9') {
			// Not a port, keep the whole string as host (e.g. an IPv6 literal).
			return true;
		}
	}

	auto const value = std::strtoul(port_str.c_str(), nullptr, 10);
	if (value == 0 || value > 65535) {
		return false;
	}

	host = host.substr(0, colon);
	port = static_cast<unsigned short>(value);
	return true;
}

}

std::string endpoint::authority() const
{
	if (uri_port() == 0) {
		return host;
	}
	return host + ":" + std::to_string(port);
}

std::string endpoint::url() const
{
	return scheme + "://" + authority();
}

unsigned short endpoint::uri_port() const
{
	if ((scheme == "https" && port == 443) || (scheme == "http" && port == 80)) {
		return 0;
	}
	return port;
}

bool parse_flag(std::string const& value, bool & out)
{
	auto const lower = to_lower(trimmed(value));
	if (lower.empty()) {
		return false;
	}

	if (lower == "1" || lower == "true" || lower == "yes" || lower == "on" || lower == "tls" || lower == "https") {
		out = true;
		return true;
	}
	if (lower == "0" || lower == "false" || lower == "no" || lower == "off" || lower == "plain" || lower == "http") {
		out = false;
		return true;
	}
	return false;
}

endpoint resolve(std::string const& configured, std::string const& tls_param, unsigned short default_port)
{
	endpoint ret;

	std::string value = trimmed(configured);

	// A full URL may be given, e.g. "http://minio.local:9000".
	bool have_scheme = false;
	auto const scheme_end = value.find("://");
	if (scheme_end != std::string::npos) {
		auto const scheme = to_lower(value.substr(0, scheme_end));
		if (scheme == "http" || scheme == "https") {
			ret.scheme = scheme;
			have_scheme = true;
		}
		value = value.substr(scheme_end + 3);
	}

	// Drop a trailing path if one was configured.
	auto const slash = value.find('/');
	if (slash != std::string::npos) {
		value = value.substr(0, slash);
	}

	unsigned short port = 0;
	split_port(value, ret.host, port);

	ret.port = port ? port : default_port;
	if (ret.port == 0) {
		ret.port = 443;
	}

	bool flag = false;
	if (parse_flag(tls_param, flag)) {
		ret.scheme = flag ? "https" : "http";
	}
	else if (!have_scheme) {
		ret.scheme = is_secure_port(ret.port) ? "https" : "http";
	}

	return ret;
}

endpoint with_host(endpoint const& base, std::string const& host)
{
	if (host.empty()) {
		return base;
	}

	endpoint ret = base;

	// Allow the override to carry its own scheme/port as well.
	std::string value = trimmed(host);
	auto const scheme_end = value.find("://");
	if (scheme_end != std::string::npos) {
		auto const scheme = to_lower(value.substr(0, scheme_end));
		if (scheme == "http" || scheme == "https") {
			ret.scheme = scheme;
		}
		value = value.substr(scheme_end + 3);
	}
	auto const slash = value.find('/');
	if (slash != std::string::npos) {
		value = value.substr(0, slash);
	}

	unsigned short port = 0;
	std::string parsed_host;
	split_port(value, parsed_host, port);
	if (!parsed_host.empty()) {
		ret.host = parsed_host;
	}
	if (port) {
		ret.port = port;
		if (scheme_end == std::string::npos) {
			// No scheme given, derive it from the port.
			ret.scheme = is_secure_port(port) ? "https" : "http";
		}
	}

	return ret;
}

std::string rebase(std::string const& url, endpoint const& ep)
{
	auto const scheme_end = url.find("://");
	if (scheme_end == std::string::npos) {
		return url;
	}
	auto const path_start = url.find('/', scheme_end + 3);
	std::string const rest = path_start == std::string::npos ? std::string() : url.substr(path_start);
	return ep.url() + rest;
}
}
