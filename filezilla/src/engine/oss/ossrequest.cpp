#include "ossrequest.h"

#if defined(HAVE_SYSTEM_PUGIXML) && HAVE_SYSTEM_PUGIXML
#include <pugixml.hpp>
#else
#include "../../pugixml/pugixml.hpp"
#endif

#include <libfilezilla/encode.hpp>
#include <libfilezilla/hash.hpp>
#include <libfilezilla/time.hpp>

#include <cstdio>
#include <vector>

#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace oss {

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

bool load(std::string_view const& xml, pugi::xml_document & doc)
{
	if (xml.empty()) {
		return false;
	}
	auto const result = doc.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata);
	return static_cast<bool>(result);
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

std::string canonicalized_oss_headers(std::map<std::string, std::string> const& headers)
{
	std::vector<std::pair<std::string, std::string>> oss_headers;
	for (auto const& h : headers) {
		auto const name = to_lower(h.first);
		if (name.size() > 6 && name.compare(0, 6, "x-oss-") == 0) {
			oss_headers.emplace_back(name, trimmed(h.second));
		}
	}
	std::sort(oss_headers.begin(), oss_headers.end());

	std::string ret;
	for (auto const& h : oss_headers) {
		ret += h.first;
		ret += ':';
		ret += h.second;
		ret += '\n';
	}
	return ret;
}

std::string canonicalized_resource(std::string const& bucket, std::string const& key,
	std::vector<std::pair<std::string, std::string>> const& subresources)
{
	std::string ret;
	if (bucket.empty()) {
		// Request against the service itself (e.g. listing buckets)
		ret = "/";
	}
	else {
		ret = "/" + bucket + "/";
		if (!key.empty()) {
			ret += key;
		}
	}

	std::vector<std::pair<std::string, std::string>> params;
	params.reserve(subresources.size());
	for (auto const& s : subresources) {
		params.emplace_back(to_lower(s.first), s.second);
	}
	std::sort(params.begin(), params.end());

	for (auto const& p : params) {
		ret += '\n';
		ret += p.first;
		if (!p.second.empty()) {
			ret += '=';
			ret += p.second;
		}
	}
	return ret;
}

std::string string_to_sign(std::string const& verb, std::map<std::string, std::string> const& headers,
	std::string const& bucket, std::string const& key,
	std::vector<std::pair<std::string, std::string>> const& subresources)
{
	std::map<std::string, std::string> lower;
	for (auto const& h : headers) {
		lower[to_lower(h.first)] = h.second;
	}

	std::string ret = verb;
	ret += '\n';
	ret += header_value(lower, "content-md5");
	ret += '\n';
	ret += header_value(lower, "content-type");
	ret += '\n';
	ret += header_value(lower, "date");
	ret += '\n';
	ret += canonicalized_oss_headers(lower);
	ret += canonicalized_resource(bucket, key, subresources);
	return ret;
}

std::string sign(credentials const& cred, std::string const& verb,
	std::map<std::string, std::string> const& headers, std::string const& bucket,
	std::string const& key, std::vector<std::pair<std::string, std::string>> const& subresources)
{
	auto const sts = string_to_sign(verb, headers, bucket, key, subresources);
	auto const mac = fz::hmac_sha1(cred.secret_key, sts);
	auto const signature = fz::base64_encode(mac);
	return "OSS " + cred.access_key + ":" + signature;
}

std::string encode_key(std::string const& key)
{
	static char const hex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(key.size() + 8);
	for (unsigned char const c : key) {
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
	if (ret.back() != '/') {
		ret += '/';
	}
	std::string n = name;
	while (!n.empty() && n.front() == '/') {
		n.erase(0, 1);
	}
	ret += n;
	return ret;
}

bool parse_list_buckets(std::string const& xml, std::vector<bucket_entry> & out)
{
	out.clear();

	pugi::xml_document doc;
	if (!load(xml, doc)) {
		return false;
	}

	auto const root = doc.child("ListAllMyBucketsResult");
	if (!root) {
		return false;
	}

	for (auto const& node : root.child("Buckets").children("Bucket")) {
		bucket_entry e;
		e.name = child_text(node, "Name");
		if (e.name.empty()) {
			continue;
		}
		e.location = child_text(node, "Location");
		e.creation_date = child_text(node, "CreationDate");
		out.push_back(std::move(e));
	}
	return true;
}

bool parse_list_objects(std::string const& xml, list_result & out)
{
	out = list_result();

	pugi::xml_document doc;
	if (!load(xml, doc)) {
		return false;
	}

	auto const root = doc.child("ListBucketResult");
	if (!root) {
		return false;
	}

	out.bucket = child_text(root, "Name");
	out.prefix = child_text(root, "Prefix");
	out.delimiter = child_text(root, "Delimiter");
	out.next_marker = child_text(root, "NextMarker");
	out.max_keys = std::strtoll(child_text(root, "MaxKeys").c_str(), nullptr, 10);
	auto const truncated = child_text(root, "IsTruncated");
	out.truncated = truncated == "true" || truncated == "True";

	for (auto const& node : root.children("Contents")) {
		object_entry e;
		e.key = child_text(node, "Key");
		if (e.key.empty()) {
			continue;
		}
		e.size = std::strtoll(child_text(node, "Size").c_str(), nullptr, 10);
		e.last_modified = child_text(node, "LastModified");
		e.etag = child_text(node, "ETag");
		e.storage_class = child_text(node, "StorageClass");
		e.is_prefix = false;
		out.entries.push_back(std::move(e));
	}

	for (auto const& node : root.children("CommonPrefixes")) {
		object_entry e;
		e.key = child_text(node, "Prefix");
		if (e.key.empty()) {
			continue;
		}
		e.is_prefix = true;
		out.entries.push_back(std::move(e));
	}

	return true;
}

bool parse_error(std::string const& xml, std::string & code, std::string & message)
{
	code.clear();
	message.clear();

	pugi::xml_document doc;
	if (!load(xml, doc)) {
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

bool parse_initiate_multipart(std::string const& xml, std::string & upload_id)
{
	upload_id.clear();

	pugi::xml_document doc;
	if (!load(xml, doc)) {
		return false;
	}

	auto const root = doc.child("InitiateMultipartUploadResult");
	if (!root) {
		return false;
	}

	upload_id = child_text(root, "UploadId");
	return !upload_id.empty();
}

std::string build_complete_multipart(std::vector<part_entry> const& parts)
{
	std::string xml = "<CompleteMultipartUpload>";
	for (auto const& p : parts) {
		xml += "<Part><PartNumber>";
		xml += std::to_string(p.number);
		xml += "</PartNumber><ETag>";
		xml += p.etag;
		xml += "</ETag></Part>";
	}
	xml += "</CompleteMultipartUpload>";
	return xml;
}

// ---------------------------------------------------------------------------
// Signature version 4 (OSS4-HMAC-SHA256)
// ---------------------------------------------------------------------------

namespace {

std::string encode_component(std::string const& value)
{
	static char const hex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size());
	for (unsigned char const c : value) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '.' || c == '_' || c == '~')
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

std::string sha256_hex_of(std::string_view const& data)
{
	auto const digest = fz::sha256(data);
	return fz::hex_encode<std::string>(digest);
}

bool is_default_sign_header(std::string const& lowercase_name)
{
	if (lowercase_name.size() >= 6 && lowercase_name.compare(0, 6, "x-oss-") == 0) {
		return true;
	}
	return lowercase_name == "content-type" || lowercase_name == "content-md5";
}

}

signature_version parse_signature_version(std::string const& value)
{
	std::string lower = to_lower(value);
	if (lower == "v1" || lower == "1" || lower == "oss" || lower == "hmac-sha1") {
		return signature_version::v1;
	}
	if (lower == "auto" || lower == "fallback") {
		return signature_version::auto_fallback;
	}
	return signature_version::v4;
}

std::string iso8601_date(int64_t unix_time)
{
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

	char buf[32];
	snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02dZ",
		static_cast<int>(year), static_cast<int>(m), static_cast<int>(d),
		static_cast<int>(secs_of_day / 3600),
		static_cast<int>((secs_of_day / 60) % 60),
		static_cast<int>(secs_of_day % 60));
	return buf;
}

std::string iso8601_now()
{
	return iso8601_date(fz::datetime::now().get_time_t());
}

std::string region_from_endpoint(std::string const& host)
{
	// Accept "oss-cn-hangzhou.aliyuncs.com" as well as
	// "bucket.oss-cn-hangzhou.aliyuncs.com".
	auto const pos = host.find("oss-");
	if (pos == std::string::npos) {
		return {};
	}

	std::string rest = host.substr(pos + 4);
	auto const end = rest.find('.');
	if (end != std::string::npos) {
		rest = rest.substr(0, end);
	}
	rest = to_lower(rest);
	if (rest.empty()) {
		return {};
	}
	return "oss-" + rest == "oss-" ? std::string() : rest;
}

v4_result sign_v4(credentials const& cred, std::string const& verb, std::string const& path,
	std::vector<std::pair<std::string, std::string>> const& query,
	std::map<std::string, std::string> const& headers,
	std::string const& region, std::string const& timestamp,
	std::vector<std::string> const& additional_headers)
{
	v4_result ret;
	ret.timestamp = timestamp;

	std::string const date = timestamp.substr(0, 8);
	ret.scope = date + "/" + region + "/oss/aliyun_v4_request";

	// Canonical URI: percent-encoded resource path, '/' kept as is.
	std::string uri = path.empty() ? std::string("/") : path;
	ret.canonical_request = verb;
	ret.canonical_request += '\n';
	ret.canonical_request += encode_key(uri);
	ret.canonical_request += '\n';

	// Canonical query string: encode, then sort by the encoded key.
	std::vector<std::pair<std::string, std::string>> encoded;
	encoded.reserve(query.size());
	for (auto const& q : query) {
		encoded.emplace_back(encode_component(q.first), encode_component(q.second));
	}
	std::sort(encoded.begin(), encoded.end());

	std::string canonical_query;
	for (auto const& q : encoded) {
		if (!canonical_query.empty()) {
			canonical_query += '&';
		}
		canonical_query += q.first;
		if (!q.second.empty()) {
			canonical_query += '=';
			canonical_query += q.second;
		}
	}
	ret.canonical_request += canonical_query;
	ret.canonical_request += '\n';

	// Which headers take part in the signature?
	std::map<std::string, std::string> lower;
	for (auto const& h : headers) {
		lower[to_lower(h.first)] = trimmed(h.second);
	}

	std::vector<std::string> additional;
	for (auto const& a : additional_headers) {
		auto const name = to_lower(a);
		if (!is_default_sign_header(name) && lower.count(name) && !lower.at(name).empty()) {
			additional.push_back(name);
		}
	}
	std::sort(additional.begin(), additional.end());
	additional.erase(std::unique(additional.begin(), additional.end()), additional.end());

	std::string canonical_headers;
	for (auto const& h : lower) {
		if (is_default_sign_header(h.first) || std::binary_search(additional.begin(), additional.end(), h.first)) {
			canonical_headers += h.first;
			canonical_headers += ':';
			canonical_headers += h.second;
			canonical_headers += '\n';
		}
	}
	ret.canonical_request += canonical_headers;
	ret.canonical_request += '\n';

	std::string canonical_additional;
	for (auto const& a : additional) {
		if (!canonical_additional.empty()) {
			canonical_additional += ';';
		}
		canonical_additional += a;
	}
	ret.canonical_request += canonical_additional;
	ret.canonical_request += '\n';
	ret.canonical_request += "UNSIGNED-PAYLOAD";

	ret.string_to_sign = "OSS4-HMAC-SHA256\n";
	ret.string_to_sign += timestamp;
	ret.string_to_sign += '\n';
	ret.string_to_sign += ret.scope;
	ret.string_to_sign += '\n';
	ret.string_to_sign += sha256_hex_of(ret.canonical_request);

	auto key = fz::hmac_sha256("aliyun_v4" + cred.secret_key, date);
	key = fz::hmac_sha256(key, region);
	key = fz::hmac_sha256(key, std::string_view("oss"));
	key = fz::hmac_sha256(key, std::string_view("aliyun_v4_request"));
	ret.signature = fz::hex_encode<std::string>(fz::hmac_sha256(key, ret.string_to_sign));

	ret.authorization = "OSS4-HMAC-SHA256 Credential=" + cred.access_key + "/" + ret.scope;
	if (!canonical_additional.empty()) {
		ret.authorization += ",AdditionalHeaders=" + canonical_additional;
	}
	ret.authorization += ",Signature=" + ret.signature;

	return ret;
}
}
