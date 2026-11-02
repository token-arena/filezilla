#ifndef FILEZILLA_ENGINE_DROPBOX_DROPBOXJSON_HEADER
#define FILEZILLA_ENGINE_DROPBOX_DROPBOXJSON_HEADER

#include <cstdint>
#include <string>
#include <vector>

// Pure logic for the Dropbox API v2.
// RPC calls live under api.dropboxapi.com, transfers under content.dropboxapi.com
// and use the Dropbox-API-Arg header, which must be plain ASCII.
// Kept free of engine dependencies (see tests/dropboxselftest.cpp).
namespace dropbox {

	struct entry {
		std::string tag;  // "file" or "folder"
		std::string name;
		std::string path_display;
		std::string id;
		int64_t size{};
		bool is_dir{false};
	};

	struct list_result {
		std::vector<entry> entries;
		std::string cursor;
		bool has_more{false};
	};

	/// Parses a files/list_folder (or .../continue) response.
	bool parse_list(std::string const& json, list_result & out, std::string & error);

	/// Parses a Dropbox error document (error_summary and/or error.*).
	bool parse_error(std::string const& json, std::string & message);

	/// Escapes every non-ASCII byte of a JSON document as \uXXXX so it can be
	/// used in an HTTP header (Dropbox requirement).
	std::string escape_non_ascii(std::string const& json);

	/// "/a" + "b.txt" -> "/a/b.txt"; root is "".
	std::string join_path(std::string const& dir, std::string const& name);

	// --- URLs ---
	std::string rpc_url(std::string const& endpoint);     // https://api.dropboxapi.com/2/<endpoint>
	std::string content_url(std::string const& endpoint); // https://content.dropboxapi.com/2/<endpoint>

	// --- request bodies (RPC) ---
	std::string list_body(std::string const& path);
	std::string continue_body(std::string const& cursor);
	std::string delete_body(std::string const& path);
	std::string create_folder_body(std::string const& path);
	std::string move_body(std::string const& from, std::string const& to);

	// --- Dropbox-API-Arg values (content endpoints) ---
	std::string download_arg(std::string const& path);
	std::string upload_arg(std::string const& path, bool overwrite);
}

#endif
