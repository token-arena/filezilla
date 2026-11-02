#include "b2json.h"

#include <libfilezilla/json.hpp>
#include <libfilezilla/time.hpp>

namespace b2 {

namespace {

std::string str(fz::json const& j, char const* name)
{
	auto const& v = j[name];
	if (!v || v.is_null()) {
		return {};
	}
	return v.string_value();
}

int64_t num(fz::json const& j, char const* name)
{
	auto const& v = j[name];
	if (!v || !v.is_number()) {
		return 0;
	}
	return v.number_value<int64_t>(0);
}

bool fail(std::string & error, std::string const& what)
{
	error = what;
	return false;
}

}

bool parse_error(std::string const& json, std::string & code, std::string & message)
{
	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return false;
	}

	auto const c = str(doc, "code");
	auto const m = str(doc, "message");
	if (c.empty() && m.empty()) {
		return false;
	}

	code = c;
	message = m;
	return true;
}

bool parse_authorize(std::string const& json, auth_info & out, std::string & error)
{
	out = auth_info();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return fail(error, "response is not a JSON object");
	}

	out.account_id = str(doc, "accountId");
	out.authorization_token = str(doc, "authorizationToken");
	if (out.account_id.empty() || out.authorization_token.empty()) {
		std::string code, message;
		if (parse_error(json, code, message)) {
			return fail(error, code + ": " + message);
		}
		return fail(error, "missing accountId or authorizationToken");
	}

	auto const& api = doc["apiInfo"]["storageApi"];
	out.api_url = str(api, "apiUrl");
	out.download_url = str(api, "downloadUrl");
	out.bucket_id = str(api, "bucketId");
	out.bucket_name = str(api, "bucketName");

	if (out.api_url.empty()) {
		return fail(error, "missing storage API url");
	}

	return true;
}

bool parse_list_file_names(std::string const& json, list_result & out, std::string & error)
{
	out = list_result();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return fail(error, "response is not a JSON object");
	}

	auto const& files = doc["files"];
	if (files && !files.is_array()) {
		return fail(error, "files is not an array");
	}

	for (size_t i = 0; i < files.children(); ++i) {
		auto const& f = files[i];
		file_entry e;
		e.file_name = str(f, "fileName");
		e.file_id = str(f, "fileId");
		e.size = num(f, "contentLength");
		e.upload_timestamp = num(f, "uploadTimestamp");
		e.action = str(f, "action");
		if (e.file_name.empty()) {
			continue;
		}
		out.files.push_back(std::move(e));
	}

	out.next_file_name = str(doc, "nextFileName");
	out.has_more = !out.next_file_name.empty();

	return true;
}

bool parse_list_buckets(std::string const& json, std::vector<bucket_entry> & out, std::string & error)
{
	out.clear();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return fail(error, "response is not a JSON object");
	}

	auto const& buckets = doc["buckets"];
	if (buckets && !buckets.is_array()) {
		return fail(error, "buckets is not an array");
	}

	for (size_t i = 0; i < buckets.children(); ++i) {
		auto const& b = buckets[i];
		bucket_entry e;
		e.bucket_id = str(b, "bucketId");
		e.bucket_name = str(b, "bucketName");
		if (e.bucket_name.empty()) {
			continue;
		}
		out.push_back(std::move(e));
	}

	return true;
}

std::string make_list_request(std::string const& bucket_id, std::string const& prefix,
	std::string const& delimiter, int max_count, std::string const& start_file_name)
{
	fz::json doc;
	doc["bucketId"] = bucket_id;
	if (!prefix.empty()) {
		doc["prefix"] = prefix;
	}
	if (!delimiter.empty()) {
		doc["delimiter"] = delimiter;
	}
	doc["maxFileCount"] = max_count;
	if (!start_file_name.empty()) {
		doc["startFileName"] = start_file_name;
	}
	return doc.to_string();
}

std::string make_delete_file_request(std::string const& file_name, std::string const& file_id)
{
	fz::json doc;
	doc["fileName"] = file_name;
	doc["fileId"] = file_id;
	return doc.to_string();
}

std::string make_copy_request(std::string const& source, std::string const& destination,
	std::string const& bucket_id)
{
	fz::json doc;
	doc["sourceFileId"] = source;
	doc["fileName"] = destination;
	doc["destinationBucketId"] = bucket_id;
	return doc.to_string();
}

std::string make_upload_url_request(std::string const& bucket_id)
{
	fz::json doc;
	doc["bucketId"] = bucket_id;
	return doc.to_string();
}

std::string encode_file_name(std::string const& name)
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
	if (!name.empty() && name.front() == '/') {
		return prefix + name;
	}
	std::string ret = prefix;
	if (ret.back() != '/') {
		ret += '/';
	}
	ret += name;
	return ret;
}

int64_t now_millis()
{
	auto const now = fz::datetime::now();
	return static_cast<int64_t>(now.get_time_t()) * 1000;
}

}
