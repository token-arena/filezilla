#ifndef FILEZILLA_ENGINE_ONEDRIVE_GRAPHJSON_HEADER
#define FILEZILLA_ENGINE_ONEDRIVE_GRAPHJSON_HEADER

#include <cstdint>
#include <string>
#include <vector>

// Pure logic for the Microsoft Graph API (OneDrive).
// The OAuth2 access token is supplied as the password and sent as a Bearer token.
// Graph supports path based addressing ("/me/drive/root:/a/b.txt:"), which maps
// directly onto FileZilla's server paths.
// Kept free of engine dependencies (see tests/graphselftest.cpp).
namespace graph {

	struct item_entry {
		std::string id;
		std::string name;
		int64_t size{};
		std::string modified;
		std::string download_url;
		std::string mime_type;
		bool is_dir{false};
	};

	struct list_result {
		std::vector<item_entry> items;
		std::string next_link;
	};

	/// Parses a children listing (value[] + @odata.nextLink).
	bool parse_list(std::string const& json, list_result & out, std::string & error);

	/// Parses a single item (driveItem) response.
	bool parse_item(std::string const& json, item_entry & out, std::string & error);

	/// Extracts error.code / error.message from a Graph error document.
	bool parse_error(std::string const& json, std::string & code, std::string & message);

	/// Percent-encodes a path for the ":<path>:" addressing form, keeping '/'.
	std::string encode_path(std::string const& path);

	// --- URL builders ---
	std::string root_children_url(int top);
	std::string item_children_url(std::string const& path, int top);
	std::string item_url(std::string const& path);
	std::string content_url(std::string const& path);
	std::string create_child_url(std::string const& parent_path);
	std::string patch_item_url(std::string const& item_id);

	// --- request bodies ---
	std::string folder_body(std::string const& name);
	std::string rename_body(std::string const& name);
	std::string move_body(std::string const& new_parent_id, std::string const& new_name);
}

#endif
