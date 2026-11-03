#include "cossign.h"

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

namespace tcos {

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

std::string sha1_hex(std::string_view const& data)
{
	return fz::hex_encode<std::string>(fz::sha1(data));
}

std::string hmac_sha1_hex(std::string_view const& key, std::string_view const& data)
{
	return fz::hex_encode<std::string>(fz::hmac_sha1(key, data));
}

std::string encode_value(std::string const& value)
{
	static char const hex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size() + 8);
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

std::string make_key_time(int64_t now, int64_t lifetime_seconds)
{
	return std::to_string(now) + ";" + std::to_string(now + lifetime_seconds);
}

std::string key_time_now()
{
	return make_key_time(fz::datetime::now().get_time_t());
}

signature sign(credentials const& cred, std::string const& method, std::string const& pathname,
	std::vector<std::pair<std::string, std::string>> const& params,
	std::vector<std::pair<std::string, std::string>> const& headers,
	std::string const& key_time)
{
	signature ret;
	ret.key_time = key_time;

	// Names are case insensitive and must be lower cased and sorted.
	std::vector<std::pair<std::string, std::string>> lower_params;
	lower_params.reserve(params.size());
	for (auto const& p : params) {
		lower_params.emplace_back(to_lower(p.first), p.second);
	}
	std::sort(lower_params.begin(), lower_params.end());

	std::vector<std::pair<std::string, std::string>> lower_headers;
	lower_headers.reserve(headers.size());
	for (auto const& h : headers) {
		lower_headers.emplace_back(to_lower(h.first), h.second);
	}
	std::sort(lower_headers.begin(), lower_headers.end());

	std::string param_string;
	for (auto const& p : lower_params) {
		if (!ret.param_list.empty()) {
			ret.param_list += ';';
			param_string += '&';
		}
		ret.param_list += p.first;
		param_string += p.first;
		param_string += '=';
		param_string += encode_value(p.second);
	}

	std::string header_string;
	for (auto const& h : lower_headers) {
		if (!ret.header_list.empty()) {
			ret.header_list += ';';
			header_string += '&';
		}
		ret.header_list += h.first;
		header_string += h.first;
		header_string += '=';
		header_string += encode_value(h.second);
	}

	std::string lower_method = to_lower(method);

	ret.http_string = lower_method;
	ret.http_string += '\n';
	ret.http_string += pathname;
	ret.http_string += '\n';
	ret.http_string += param_string;
	ret.http_string += '\n';
	ret.http_string += header_string;
	ret.http_string += '\n';

	ret.string_to_sign = "sha1\n";
	ret.string_to_sign += key_time;
	ret.string_to_sign += '\n';
	ret.string_to_sign += sha1_hex(ret.http_string);
	ret.string_to_sign += '\n';

	auto const sign_key = hmac_sha1_hex(cred.secret_key, key_time);
	ret.signature = hmac_sha1_hex(sign_key, ret.string_to_sign);

	ret.authorization = "q-sign-algorithm=sha1&q-ak=" + cred.secret_id +
		"&q-sign-time=" + key_time + "&q-key-time=" + key_time +
		"&q-header-list=" + ret.header_list +
		"&q-url-param-list=" + ret.param_list +
		"&q-signature=" + ret.signature;

	return ret;
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

	out.name = child_text(root, "Name");
	out.prefix = child_text(root, "Prefix");
	out.delimiter = child_text(root, "Delimiter");
	out.next_marker = child_text(root, "NextMarker");
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
