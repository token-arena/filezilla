#include "gcsjson.h"

#include <libfilezilla/json.hpp>

#include <cstdlib>

namespace gcs {

namespace {

std::string str(fz::json const& j, char const* name)
{
	auto const& v = j[name];
	if (!v || v.is_null()) {
		return {};
	}
	return v.string_value();
}

bool fail(std::string & error, std::string const& what)
{
	error = what;
	return false;
}

}

bool parse_error(std::string const& json, std::string & message)
{
	message.clear();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return false;
	}

	auto const& err = doc["error"];
	if (!err || !err.is_object()) {
		return false;
	}

	message = str(err, "message");
	if (message.empty()) {
		message = str(doc, "error_description");
	}
	return !message.empty();
}

bool parse_list_objects(std::string const& json, list_objects_result & out, std::string & error)
{
	out = list_objects_result();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return fail(error, "response is not a JSON object");
	}

	auto const& items = doc["items"];
	if (items && !items.is_array()) {
		return fail(error, "items is not an array");
	}

	for (size_t i = 0; i < items.children(); ++i) {
		auto const& o = items[i];
		object_entry e;
		e.name = str(o, "name");
		if (e.name.empty()) {
			continue;
		}
		// sizes are returned as strings in the JSON API
		e.size = std::strtoll(str(o, "size").c_str(), nullptr, 10);
		e.updated = str(o, "updated");
		e.content_type = str(o, "contentType");
		e.is_prefix = false;
		out.objects.push_back(std::move(e));
	}

	auto const& prefixes = doc["prefixes"];
	if (prefixes && prefixes.is_array()) {
		for (size_t i = 0; i < prefixes.children(); ++i) {
			object_entry e;
			e.name = prefixes[i].string_value();
			if (e.name.empty()) {
				continue;
			}
			e.is_prefix = true;
			out.objects.push_back(std::move(e));
		}
	}

	out.next_page_token = str(doc, "nextPageToken");
	return true;
}

bool parse_list_buckets(std::string const& json, std::vector<bucket_entry> & out, std::string & error)
{
	out.clear();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return fail(error, "response is not a JSON object");
	}

	auto const& items = doc["items"];
	if (items && !items.is_array()) {
		return fail(error, "items is not an array");
	}

	for (size_t i = 0; i < items.children(); ++i) {
		bucket_entry e;
		e.name = str(items[i], "name");
		if (e.name.empty()) {
			continue;
		}
		e.location = str(items[i], "location");
		out.push_back(std::move(e));
	}
	return true;
}

std::string encode_object_name(std::string const& name)
{
	static char const hex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(name.size() + 8);
	for (unsigned char const c : name) {
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

std::string list_objects_url(std::string const& bucket, std::string const& prefix,
	bool with_delimiter, std::string const& page_token)
{
	std::string url = "https://storage.googleapis.com/storage/v1/b/" + encode_object_name(bucket) + "/o?maxResults=1000";
	if (!prefix.empty()) {
		url += "&prefix=" + encode_object_name(prefix);
	}
	if (with_delimiter) {
		url += "&delimiter=%2F";
	}
	if (!page_token.empty()) {
		url += "&pageToken=" + encode_object_name(page_token);
	}
	return url;
}

std::string object_url(std::string const& bucket, std::string const& object)
{
	return "https://storage.googleapis.com/storage/v1/b/" + encode_object_name(bucket) + "/o/" +
		encode_object_name(object) + "?alt=media";
}

std::string object_json_url(std::string const& bucket, std::string const& object)
{
	return "https://storage.googleapis.com/storage/v1/b/" + encode_object_name(bucket) + "/o/" +
		encode_object_name(object);
}

std::string upload_url(std::string const& bucket, std::string const& object)
{
	return "https://storage.googleapis.com/upload/storage/v1/b/" + encode_object_name(bucket) +
		"/o?uploadType=media&name=" + encode_object_name(object);
}

}
