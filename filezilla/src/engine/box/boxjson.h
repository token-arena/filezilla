#ifndef FILEZILLA_ENGINE_BOX_BOXJSON_HEADER
#define FILEZILLA_ENGINE_BOX_BOXJSON_HEADER

#include <cstdint>
#include <string>
#include <vector>

// Pure logic for the Box API v2.
// The OAuth2 access token is supplied as the password and sent as a Bearer token.
// Kept free of engine dependencies (see tests/boxselftest.cpp).
namespace box {

	/// The Box root folder is addressed by the well known id "0".
	extern char const* const root_folder_id;

	struct item_entry {
		std::string type; // "file" or "folder"
		std::string id;
		std::string name;
		int64_t size{};
		std::string modified;
		bool is_dir{false};
	};

	struct list_result {
		std::vector<item_entry> items;
		int64_t total_count{};
		int64_t offset{};
	};

	/// Parses a folders/{id}/items response.
	bool parse_list(std::string const& json, list_result & out, std::string & error);

	/// Parses a single file or folder object.
	bool parse_item(std::string const& json, item_entry & out, std::string & error);

	/// Extracts code/message from a Box error document.
	bool parse_error(std::string const& json, std::string & code, std::string & message);

	// --- URL builders ---
	std::string folder_items_url(std::string const& folder_id, int64_t offset, int limit);
	std::string folder_url(std::string const& folder_id);
	std::string file_url(std::string const& file_id);
	std::string file_content_url(std::string const& file_id);
	std::string new_file_version_url(std::string const& file_id);

	// --- request bodies ---
	std::string create_folder_body(std::string const& name, std::string const& parent_id);
	std::string rename_body(std::string const& name);
	std::string move_body(std::string const& parent_id);
}

#endif
