#include "drivejson.h"

#include <libfilezilla/json.hpp>

#include <cstdlib>

namespace gdrive {

char const* const folder_mime_type = "application/vnd.google-apps.folder";

namespace {

char const* const api_base = "https://www.googleapis.com/drive/v3";
char const* const upload_base = "https://www.googleapis.com/upload/drive/v3";

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

bool parse_list_files(std::string const& json, std::vector<file_entry> & out,
	std::string & next_page_token, std::string & error)
{
	out.clear();
	next_page_token.clear();

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
		e.id = str(f, "id");
		e.name = str(f, "name");
		e.mime_type = str(f, "mimeType");
		e.size = std::strtoll(str(f, "size").c_str(), nullptr, 10);
		e.modified_time = str(f, "modifiedTime");
		e.is_dir = e.mime_type == folder_mime_type;
		if (e.id.empty()) {
			continue;
		}
		out.push_back(std::move(e));
	}

	next_page_token = str(doc, "nextPageToken");
	return true;
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
	return !message.empty();
}

std::string escape_query_value(std::string const& value)
{
	std::string out;
	out.reserve(value.size());
	for (char const c : value) {
		if (c == '\\' || c == '\'') {
			out.push_back('\\');
		}
		out.push_back(c);
	}
	return out;
}

std::string folder_query(std::string const& parent_id, std::string const& name)
{
	return "'" + escape_query_value(parent_id) + "' in parents and name = '" + escape_query_value(name) +
		"' and mimeType = '" + folder_mime_type + "' and trashed = false";
}

std::string children_query(std::string const& parent_id)
{
	return "'" + escape_query_value(parent_id) + "' in parents and trashed = false";
}

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

std::string list_url(std::string const& parent_id, std::string const& page_token)
{
	std::string url = std::string(api_base) + "/files?q=" + encode_component(children_query(parent_id)) +
		"&fields=" + encode_component("nextPageToken,files(id,name,mimeType,size,modifiedTime)") + "&pageSize=1000";
	if (!page_token.empty()) {
		url += "&pageToken=" + encode_component(page_token);
	}
	return url;
}

std::string search_url(std::string const& query)
{
	return std::string(api_base) + "/files?q=" + encode_component(query) +
		"&fields=" + encode_component("files(id,name,mimeType)") + "&pageSize=10";
}

std::string file_url(std::string const& file_id)
{
	return std::string(api_base) + "/files/" + encode_component(file_id);
}

std::string media_url(std::string const& file_id)
{
	return file_url(file_id) + "?alt=media";
}

std::string upload_url(std::string const& file_id)
{
	return std::string(upload_base) + "/files/" + encode_component(file_id) + "?uploadType=media";
}

std::string create_url()
{
	return std::string(api_base) + "/files";
}

std::string delete_url(std::string const& file_id)
{
	return file_url(file_id);
}

std::string patch_url(std::string const& file_id)
{
	return file_url(file_id);
}

std::string create_folder_body(std::string const& name, std::string const& parent_id)
{
	fz::json doc;
	doc["name"] = name;
	doc["mimeType"] = folder_mime_type;
	fz::json parents;
	parents[0] = parent_id;
	doc["parents"] = parents;
	return doc.to_string();
}

std::string create_file_body(std::string const& name, std::string const& parent_id)
{
	fz::json doc;
	doc["name"] = name;
	fz::json parents;
	parents[0] = parent_id;
	doc["parents"] = parents;
	return doc.to_string();
}

std::string rename_body(std::string const& name)
{
	fz::json doc;
	doc["name"] = name;
	return doc.to_string();
}

std::string move_body(std::string const& add_parent, std::string const& remove_parent)
{
	fz::json doc;
	doc["addParents"] = add_parent;
	doc["removeParents"] = remove_parent;
	return doc.to_string();
}

}
