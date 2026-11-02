#ifndef FILEZILLA_ENGINE_GDRIVE_DRIVEJSON_HEADER
#define FILEZILLA_ENGINE_GDRIVE_DRIVEJSON_HEADER

#include <cstdint>
#include <string>
#include <vector>

// Pure logic for the Google Drive API v3.
// The OAuth2 access token is supplied as the password and sent as a Bearer token.
// Kept free of engine dependencies for standalone unit testing
// (see tests/driveselftest.cpp).
namespace gdrive {

	/// Google's well known mime type for folders.
	extern char const* const folder_mime_type;

	struct file_entry {
		std::string id;
		std::string name;
		std::string mime_type;
		int64_t size{};
		std::string modified_time;
		bool is_dir{false};
	};

	/// Parses a files.list response.
	bool parse_list_files(std::string const& json, std::vector<file_entry> & out,
		std::string & next_page_token, std::string & error);

	/// Extracts error.message from a Google API error document.
	bool parse_error(std::string const& json, std::string & message);

	/// Escapes a value for use inside a Drive query string (name='...').
	std::string escape_query_value(std::string const& value);

	/// q expression matching a folder of the given name inside the given parent.
	std::string folder_query(std::string const& parent_id, std::string const& name);

	/// q expression matching any non-trashed child of the given parent.
	std::string children_query(std::string const& parent_id);

	// --- URL builders ---

	std::string list_url(std::string const& parent_id, std::string const& page_token);
	std::string search_url(std::string const& query);
	std::string file_url(std::string const& file_id);
	std::string media_url(std::string const& file_id);
	std::string upload_url(std::string const& file_id);
	std::string create_url();
	std::string delete_url(std::string const& file_id);
	std::string patch_url(std::string const& file_id);

	// --- request bodies ---

	std::string create_folder_body(std::string const& name, std::string const& parent_id);
	std::string create_file_body(std::string const& name, std::string const& parent_id);
	std::string rename_body(std::string const& name);
	std::string move_body(std::string const& add_parent, std::string const& remove_parent);

	/// Percent-encodes a single path segment/query value.
	std::string encode_component(std::string const& value);
}

#endif
