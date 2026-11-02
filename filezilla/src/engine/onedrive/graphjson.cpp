#include "graphjson.h"

#include <libfilezilla/json.hpp>

#include <cstdlib>

namespace graph {

namespace {

char const* const api_base = "https://graph.microsoft.com/v1.0";

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

void fill_common(fz::json const& j, item_entry & e)
{
	e.id = str(j, "id");
	e.name = str(j, "name");
	e.size = std::strtoll(str(j, "size").c_str(), nullptr, 10);
	e.modified = str(j, "lastModifiedDateTime");
	e.download_url = str(j, "@microsoft.graph.downloadUrl");

	auto const& folder = j["folder"];
	e.is_dir = folder && folder.is_object();
	if (!e.is_dir) {
		auto const& file = j["file"];
		if (file && file.is_object()) {
			e.mime_type = str(file, "mimeType");
		}
	}
}

}

bool parse_list(std::string const& json, list_result & out, std::string & error)
{
	out = list_result();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return fail(error, "response is not a JSON object");
	}

	auto const& value = doc["value"];
	if (value && !value.is_array()) {
		return fail(error, "value is not an array");
	}

	for (size_t i = 0; i < value.children(); ++i) {
		item_entry e;
		fill_common(value[i], e);
		if (e.id.empty() && e.name.empty()) {
			continue;
		}
		out.items.push_back(std::move(e));
	}

	out.next_link = str(doc, "@odata.nextLink");
	return true;
}

bool parse_item(std::string const& json, item_entry & out, std::string & error)
{
	out = item_entry();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return fail(error, "response is not a JSON object");
	}

	fill_common(doc, out);
	if (out.id.empty() && out.name.empty()) {
		return fail(error, "item is missing id and name");
	}
	return true;
}

bool parse_error(std::string const& json, std::string & code, std::string & message)
{
	code.clear();
	message.clear();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return false;
	}

	auto const& err = doc["error"];
	if (!err || !err.is_object()) {
		return false;
	}

	code = str(err, "code");
	message = str(err, "message");
	return !code.empty() || !message.empty();
}

std::string encode_path(std::string const& path)
{
	static char const hex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(path.size() + 8);
	for (unsigned char const c : path) {
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

std::string root_children_url(int top)
{
	return std::string(api_base) + "/me/drive/root/children?$top=" + std::to_string(top);
}

std::string item_children_url(std::string const& path, int top)
{
	if (path.empty()) {
		return root_children_url(top);
	}
	std::string p = path;
	if (p.front() != '/') {
		p.insert(p.begin(), '/');
	}
	return std::string(api_base) + "/me/drive/root:" + encode_path(p) + ":/children?$top=" + std::to_string(top);
}

std::string item_url(std::string const& path)
{
	if (path.empty() || path == "/") {
		return std::string(api_base) + "/me/drive/root";
	}
	std::string p = path;
	if (p.front() != '/') {
		p.insert(p.begin(), '/');
	}
	return std::string(api_base) + "/me/drive/root:" + encode_path(p) + ":";
}

std::string content_url(std::string const& path)
{
	return item_url(path) + "/content";
}

std::string create_child_url(std::string const& parent_path)
{
	if (parent_path.empty() || parent_path == "/") {
		return std::string(api_base) + "/me/drive/root/children";
	}
	std::string p = parent_path;
	if (p.front() != '/') {
		p.insert(p.begin(), '/');
	}
	return std::string(api_base) + "/me/drive/root:" + encode_path(p) + ":/children";
}

std::string patch_item_url(std::string const& item_id)
{
	return std::string(api_base) + "/me/drive/items/" + encode_path(item_id);
}

std::string folder_body(std::string const& name)
{
	fz::json doc;
	doc["name"] = name;
	fz::json folder;
	folder["childCount"] = 0;
	doc["folder"] = folder;
	doc["@microsoft.graph.conflictBehavior"] = "rename";
	return doc.to_string();
}

std::string rename_body(std::string const& name)
{
	fz::json doc;
	doc["name"] = name;
	return doc.to_string();
}

std::string move_body(std::string const& new_parent_id, std::string const& new_name)
{
	fz::json doc;
	if (!new_name.empty()) {
		doc["name"] = new_name;
	}
	if (!new_parent_id.empty()) {
		fz::json parent;
		parent["id"] = new_parent_id;
		doc["parentReference"] = parent;
	}
	return doc.to_string();
}

}
