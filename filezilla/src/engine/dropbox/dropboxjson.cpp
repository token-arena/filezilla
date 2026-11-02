#include "dropboxjson.h"

#include <libfilezilla/json.hpp>

#include <cstdio>
#include <cstdlib>

namespace dropbox {

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

bool parse_list(std::string const& json, list_result & out, std::string & error)
{
	out = list_result();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return fail(error, "response is not a JSON object");
	}

	auto const& entries = doc["entries"];
	if (entries && !entries.is_array()) {
		return fail(error, "entries is not an array");
	}

	for (size_t i = 0; i < entries.children(); ++i) {
		auto const& j = entries[i];
		entry e;
		e.tag = str(j, ".tag");
		e.name = str(j, "name");
		e.path_display = str(j, "path_display");
		e.id = str(j, "id");
		e.size = std::strtoll(str(j, "size").c_str(), nullptr, 10);
		e.is_dir = e.tag == "folder";
		if (e.name.empty()) {
			continue;
		}
		out.entries.push_back(std::move(e));
	}

	out.cursor = str(doc, "cursor");
	out.has_more = doc["has_more"].bool_value();

	return true;
}

bool parse_error(std::string const& json, std::string & message)
{
	message.clear();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return false;
	}

	message = str(doc, "error_summary");
	if (message.empty()) {
		auto const& err = doc["error"];
		if (err && err.is_object()) {
			message = str(err, ".tag");
		}
	}
	return !message.empty();
}

std::string escape_non_ascii(std::string const& json)
{
	std::string out;
	out.reserve(json.size());

	for (size_t i = 0; i < json.size(); ) {
		unsigned char const c = static_cast<unsigned char>(json[i]);
		if (c < 0x80) {
			out.push_back(static_cast<char>(c));
			++i;
			continue;
		}

		// Decode one UTF-8 sequence
		unsigned int cp = 0;
		size_t len = 0;
		if ((c & 0xe0) == 0xc0) {
			cp = c & 0x1f;
			len = 2;
		}
		else if ((c & 0xf0) == 0xe0) {
			cp = c & 0x0f;
			len = 3;
		}
		else if ((c & 0xf8) == 0xf0) {
			cp = c & 0x07;
			len = 4;
		}
		else {
			// Invalid, replace
			out += "\\ufffd";
			++i;
			continue;
		}

		if (i + len > json.size()) {
			out += "\\ufffd";
			break;
		}

		for (size_t k = 1; k < len; ++k) {
			cp = (cp << 6) | (static_cast<unsigned char>(json[i + k]) & 0x3f);
		}
		i += len;

		char buf[8];
		if (cp <= 0xffff) {
			snprintf(buf, sizeof(buf), "\\u%04x", cp);
			out += buf;
		}
		else {
			cp -= 0x10000;
			snprintf(buf, sizeof(buf), "\\u%04x", 0xd800 + (cp >> 10));
			out += buf;
			snprintf(buf, sizeof(buf), "\\u%04x", 0xdc00 + (cp & 0x3ff));
			out += buf;
		}
	}
	return out;
}

std::string join_path(std::string const& dir, std::string const& name)
{
	if (dir.empty() || dir == "/") {
		return "/" + name;
	}
	std::string ret = dir;
	if (ret.back() != '/') {
		ret += '/';
	}
	ret += name;
	return ret;
}

std::string rpc_url(std::string const& endpoint)
{
	return "https://api.dropboxapi.com/2/" + endpoint;
}

std::string content_url(std::string const& endpoint)
{
	return "https://content.dropboxapi.com/2/" + endpoint;
}

std::string list_body(std::string const& path)
{
	fz::json doc;
	doc["path"] = path;
	doc["recursive"] = false;
	doc["include_media_info"] = false;
	doc["include_deleted"] = false;
	doc["include_has_explicit_shared_members"] = false;
	doc["limit"] = 1000;
	return doc.to_string();
}

std::string continue_body(std::string const& cursor)
{
	fz::json doc;
	doc["cursor"] = cursor;
	return doc.to_string();
}

std::string delete_body(std::string const& path)
{
	fz::json doc;
	doc["path"] = path;
	return doc.to_string();
}

std::string create_folder_body(std::string const& path)
{
	fz::json doc;
	doc["path"] = path;
	doc["autorename"] = false;
	return doc.to_string();
}

std::string move_body(std::string const& from, std::string const& to)
{
	fz::json doc;
	doc["from_path"] = from;
	doc["to_path"] = to;
	doc["autorename"] = false;
	doc["allow_ownership_transfer"] = false;
	return doc.to_string();
}

std::string download_arg(std::string const& path)
{
	fz::json doc;
	doc["path"] = path;
	return escape_non_ascii(doc.to_string());
}

std::string upload_arg(std::string const& path, bool overwrite)
{
	fz::json doc;
	doc["path"] = path;
	doc["mode"] = overwrite ? "overwrite" : "add";
	doc["autorename"] = false;
	doc["mute"] = false;
	doc["strict_conflict"] = false;
	return escape_non_ascii(doc.to_string());
}

}
