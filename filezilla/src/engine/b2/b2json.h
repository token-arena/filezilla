#ifndef FILEZILLA_ENGINE_B2_B2JSON_HEADER
#define FILEZILLA_ENGINE_B2_B2JSON_HEADER

#include <cstdint>
#include <string>
#include <vector>

// Pure logic for the Backblaze B2 native API (b2api/v3, JSON over HTTPS).
// Kept free of engine dependencies so it can be unit-tested standalone
// (see tests/b2selftest.cpp).
namespace b2 {

	struct auth_info {
		std::string account_id;
		std::string authorization_token;
		std::string api_url;      // storage API base URL
		std::string download_url; // base URL for downloading files
		std::string bucket_id;    // set when the key is restricted to a single bucket
		std::string bucket_name;
	};

	struct file_entry {
		std::string file_name;
		std::string file_id;
		int64_t size{};
		int64_t upload_timestamp{}; // milliseconds since epoch
		std::string action;         // "upload", "folder" or "hide"
	};

	struct list_result {
		std::vector<file_entry> files;
		std::string next_file_name;
		bool has_more{false};
	};

	struct bucket_entry {
		std::string bucket_id;
		std::string bucket_name;
	};

	/// Parses a b2_authorize_account response. Returns false and fills error on failure.
	bool parse_authorize(std::string const& json, auth_info & out, std::string & error);

	/// Parses a b2_list_file_names response.
	bool parse_list_file_names(std::string const& json, list_result & out, std::string & error);

	/// Parses a b2_list_buckets response.
	bool parse_list_buckets(std::string const& json, std::vector<bucket_entry> & out, std::string & error);

	/// Extracts code/message out of a B2 error document. Returns false if it is not one.
	bool parse_error(std::string const& json, std::string & code, std::string & message);

	/// Request bodies.
	std::string make_list_request(std::string const& bucket_id, std::string const& prefix,
		std::string const& delimiter, int max_count, std::string const& start_file_name);
	std::string make_delete_file_request(std::string const& file_name, std::string const& file_id);
	std::string make_copy_request(std::string const& source, std::string const& destination,
		std::string const& bucket_id);
	std::string make_upload_url_request(std::string const& bucket_id);

	/// Percent-encodes a file name for the X-Bz-File-Name header (keeps '/', encodes
	/// everything outside the unreserved set).
	std::string encode_file_name(std::string const& name);

	/// Joins a directory prefix and an object name, ensuring exactly one '/'.
	std::string join_name(std::string const& prefix, std::string const& name);

	/// Converts an ISO8601 timestamp in milliseconds since epoch to a B2 upload timestamp.
	int64_t now_millis();
}

#endif
