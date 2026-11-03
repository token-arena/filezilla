#include "ossrequest.h"

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

}
