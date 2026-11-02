#include "azureblob.h"

#if defined(HAVE_SYSTEM_PUGIXML) && HAVE_SYSTEM_PUGIXML
#include <pugixml.hpp>
#else
#include "../../pugixml/pugixml.hpp"
#endif

#include <libfilezilla/encode.hpp>
#include <libfilezilla/hash.hpp>
#include <libfilezilla/time.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace azure {

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

std::string header_value(std::map<std::string, std::string> const& headers, char const* name)
{
	auto const it = headers.find(name);
	if (it == headers.end()) {
		return {};
	}
	return trimmed(it->second);
}

std::string child_text(pugi::xml_node const& parent, char const* name)
{
	auto const child = parent.child(name);
	if (!child) {
		return {};
	}
	return child.child_value();
}

}

std::string canonicalized_headers(std::map<std::string, std::string> const& headers)
{
	std::vector<std::pair<std::string, std::string>> ms;
	for (auto const& h : headers) {
		auto const name = to_lower(h.first);
		if (name.size() > 5 && name.compare(0, 5, "x-ms-") == 0) {
			ms.emplace_back(name, trimmed(h.second));
		}
	}
	std::sort(ms.begin(), ms.end());

	std::string ret;
	for (auto const& h : ms) {
		ret += h.first;
		ret += ':';
		ret += h.second;
		ret += '\n';
	}
	return ret;
}

std::string canonicalized_resource(std::string const& account, std::string const& path,
	std::vector<std::pair<std::string, std::string>> const& query)
{
	std::string ret = "/" + account + path;

	std::vector<std::pair<std::string, std::string>> params;
	params.reserve(query.size());
	for (auto const& q : query) {
		params.emplace_back(to_lower(q.first), q.second);
	}
	std::sort(params.begin(), params.end());

	for (auto const& q : params) {
		ret += '\n';
		ret += q.first;
		ret += ':';
		ret += q.second;
	}
	return ret;
}

std::string string_to_sign(std::string const& verb, std::map<std::string, std::string> const& headers,
	std::string const& account, std::string const& path,
	std::vector<std::pair<std::string, std::string>> const& query)
{
	// Header names are case insensitive: normalize before looking anything up.
	std::map<std::string, std::string> lower;
	for (auto const& h : headers) {
		lower[to_lower(h.first)] = h.second;
	}

	std::string content_length = header_value(lower, "content-length");
	if (content_length == "0") {
		content_length.clear();
	}

	static char const* const fixed_fields[] = {
		"content-encoding",
		"content-language",
		nullptr,
		"content-md5",
		"content-type",
		"date",
		"if-modified-since",
		"if-match",
		"if-none-match",
		"if-unmodified-since",
		"range",
	};

	std::string ret = verb;
	ret += '\n';

	for (auto name : fixed_fields) {
		if (name == nullptr) {
			ret += content_length;
		}
		else {
			ret += header_value(lower, name);
		}
		ret += '\n';
	}

	ret += canonicalized_headers(lower);
	ret += canonicalized_resource(account, path, query);
	return ret;
}

std::string sign(credentials const& cred, std::string const& verb,
	std::map<std::string, std::string> const& headers, std::string const& path,
	std::vector<std::pair<std::string, std::string>> const& query)
{
	auto const sts = string_to_sign(verb, headers, cred.account, path, query);

	auto const key = fz::base64_decode(cred.key_base64);
	auto const mac = fz::hmac_sha256(key, sts);
	auto const signature = fz::base64_encode(mac);

	return "SharedKey " + cred.account + ":" + signature;
}

bool parse_list_blobs(std::string const& xml, list_blobs_result & out)
{
	out = list_blobs_result();
	if (xml.empty()) {
		return false;
	}

	pugi::xml_document doc;
	if (!doc.load_buffer(xml.data(), xml.size())) {
		return false;
	}

	auto const root = doc.child("EnumerationResults");
	if (!root) {
		return false;
	}

	auto const blobs = root.child("Blobs");
	if (blobs) {
		for (auto const& node : blobs.children("Blob")) {
			blob_entry entry;
			entry.name = child_text(node, "Name");
			if (entry.name.empty()) {
				continue;
			}
			auto const props = node.child("Properties");
			entry.size = props ? std::strtoll(child_text(props, "Content-Length").c_str(), nullptr, 10) : 0;
			entry.last_modified = props ? child_text(props, "Last-Modified") : std::string();
			entry.content_type = props ? child_text(props, "Content-Type") : std::string();
			entry.is_prefix = false;
			out.blobs.push_back(std::move(entry));
		}

		for (auto const& node : blobs.children("BlobPrefix")) {
			blob_entry entry;
			entry.name = child_text(node, "Name");
			if (entry.name.empty()) {
				continue;
			}
			entry.is_prefix = true;
			out.blobs.push_back(std::move(entry));
		}
	}

	out.next_marker = child_text(root, "NextMarker");
	return true;
}

bool parse_list_containers(std::string const& xml, std::vector<container_entry> & out,
	std::string & next_marker)
{
	out.clear();
	next_marker.clear();
	if (xml.empty()) {
		return false;
	}

	pugi::xml_document doc;
	if (!doc.load_buffer(xml.data(), xml.size())) {
		return false;
	}

	auto const root = doc.child("EnumerationResults");
	if (!root) {
		return false;
	}

	auto const containers = root.child("Containers");
	if (containers) {
		for (auto const& node : containers.children("Container")) {
			container_entry entry;
			entry.name = child_text(node, "Name");
			if (entry.name.empty()) {
				continue;
			}
			auto const props = node.child("Properties");
			entry.last_modified = props ? child_text(props, "Last-Modified") : std::string();
			out.push_back(std::move(entry));
		}
	}

	next_marker = child_text(root, "NextMarker");
	return true;
}

bool parse_error(std::string const& xml, std::string & code, std::string & message)
{
	code.clear();
	message.clear();
	if (xml.empty()) {
		return false;
	}

	pugi::xml_document doc;
	if (!doc.load_buffer(xml.data(), xml.size())) {
		return false;
	}

	auto const root = doc.child("Error");
	if (!root) {
		return false;
	}

	code = child_text(root, "Code");
	message = child_text(root, "Message");
	return !code.empty() || !message.empty();
}

std::string rfc1123_date(int64_t unix_time)
{
	static char const* const weekday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static char const* const month[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

	uint64_t const t = unix_time < 0 ? 0 : static_cast<uint64_t>(unix_time);
	uint64_t const days_total = t / 86400;
	uint64_t const secs_of_day = t % 86400;

	int64_t z = static_cast<int64_t>(days_total) + 719468;
	int64_t const era = (z >= 0 ? z : z - 146096) / 146097;
	uint64_t const doe = static_cast<uint64_t>(z - era * 146097);
	uint64_t const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t const y = static_cast<int64_t>(yoe) + era * 400;
	uint64_t const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint64_t const mp = (5 * doy + 2) / 153;
	uint64_t const d = doy - (153 * mp + 2) / 5 + 1;
	uint64_t const m = mp < 10 ? mp + 3 : mp - 9;
	int64_t const year = y + (m <= 2 ? 1 : 0);

	// 1970-01-01 was a Thursday (index 4).
	unsigned const dow = static_cast<unsigned>((days_total + 4) % 7);

	char buf[64];
	snprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT",
		weekday[dow], static_cast<int>(d), month[m - 1], static_cast<int>(year),
		static_cast<int>(secs_of_day / 3600),
		static_cast<int>((secs_of_day / 60) % 60),
		static_cast<int>(secs_of_day % 60));
	return buf;
}

std::string rfc1123_now()
{
	return rfc1123_date(fz::datetime::now().get_time_t());
}

std::string encode_blob_name(std::string const& name)
{
	static char const hex[] = "0123456789ABCDEF";

	std::string out;
	out.reserve(name.size());
	for (unsigned char const c : name) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '.' || c == '_' || c == '~' || c == '/')
		{
			out.push_back(static_cast<char>(c));
		}
		else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0x0f]);
		}
	}
	return out;
}

std::string join_name(std::string const& prefix, std::string const& name)
{
	if (prefix.empty()) {
		return name;
	}
	std::string ret = prefix;
	if (!ret.empty() && ret.back() != '/') {
		ret += '/';
	}
	std::string n = name;
	while (!n.empty() && n.front() == '/') {
		n.erase(0, 1);
	}
	ret += n;
	return ret;
}

}
