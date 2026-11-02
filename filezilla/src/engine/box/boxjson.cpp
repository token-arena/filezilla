#include "boxjson.h"

#include <libfilezilla/json.hpp>

#include <cstdlib>

namespace box {

char const* const root_folder_id = "0";

namespace {

char const* const api_base = "https://api.box.com/2.0";
char const* const upload_base = "https://upload.box.com/api/2.0";

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

void fill(fz::json const& j, item_entry & e)
{
	e.type = str(j, "type");
	e.id = str(j, "id");
	e.name = str(j, "name");
	e.size = std::strtoll(str(j, "size").c_str(), nullptr, 10);
	e.modified = str(j, "modified_at");
	if (e.modified.empty()) {
		e.modified = str(j, "created_at");
	}
	e.is_dir = e.type == "folder";
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
		item_entry e;
		fill(entries[i], e);
		if (e.id.empty() || e.name.empty()) {
			continue;
		}
		out.items.push_back(std::move(e));
	}

	out.total_count = doc["total_count"].number_value<int64_t>(0);
	out.offset = doc["offset"].number_value<int64_t>(0);
	return true;
}

bool parse_item(std::string const& json, item_entry & out, std::string & error)
{
	out = item_entry();

	auto const doc = fz::json::parse(json);
	if (!doc.is_object()) {
		return fail(error, "response is not a JSON object");
	}

	fill(doc, out);
	if (out.id.empty()) {
		return fail(error, "item is missing an id");
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

	code = str(doc, "code");
	message = str(doc, "message");
	if (code.empty() && message.empty()) {
		auto const& err = doc["error"];
		if (err && err.is_object()) {
			code = str(err, "code");
			message = str(err, "message");
		}
	}
	return !code.empty() || !message.empty();
}

std::string folder_items_url(std::string const& folder_id, int64_t offset, int limit)
{
	std::string const id = folder_id.empty() ? std::string(root_folder_id) : folder_id;
	return std::string(api_base) + "/folders/" + id + "/items?limit=" + std::to_string(limit) +
		"&offset=" + std::to_string(offset) +
		"&fields=type,id,name,size,modified_at,created_at";
}

std::string folder_url(std::string const& folder_id)
{
	std::string const id = folder_id.empty() ? std::string(root_folder_id) : folder_id;
	return std::string(api_base) + "/folders/" + id;
}

std::string file_url(std::string const& file_id)
{
	return std::string(api_base) + "/files/" + file_id;
}

std::string file_content_url(std::string const& file_id)
{
	return std::string(api_base) + "/files/" + file_id + "/content";
}

std::string new_file_version_url(std::string const& file_id)
{
	return std::string(upload_base) + "/files/" + file_id + "/content";
}

std::string create_folder_body(std::string const& name, std::string const& parent_id)
{
	fz::json parent;
	parent["id"] = parent_id.empty() ? std::string(root_folder_id) : parent_id;

	fz::json doc;
	doc["name"] = name;
	doc["parent"] = parent;
	return doc.to_string();
}

std::string rename_body(std::string const& name)
{
	fz::json doc;
	doc["name"] = name;
	return doc.to_string();
}

std::string move_body(std::string const& parent_id)
{
	fz::json parent;
	parent["id"] = parent_id.empty() ? std::string(root_folder_id) : parent_id;

	fz::json doc;
	doc["parent"] = parent;
	return doc.to_string();
}

}
