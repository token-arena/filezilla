#ifndef FILEZILLA_ENGINE_AZURE_AZUREBLOB_HEADER
#define FILEZILLA_ENGINE_AZURE_AZUREBLOB_HEADER

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Pure logic for the Azure Blob Storage REST API (SharedKey authorization).
// Kept free of engine dependencies so it can be unit-tested standalone
// (see tests/azureselftest.cpp).
namespace azure {

	struct credentials {
		std::string account;
		std::string key_base64;
	};

	/// Lowercase x-ms-* headers, trimmed values, sorted by name, one per line.
	std::string canonicalized_headers(std::map<std::string, std::string> const& headers);

	/// "/account/path" followed by one "\nname:value" line per query parameter,
	/// sorted by (lowercase) name.
	std::string canonicalized_resource(std::string const& account, std::string const& path,
		std::vector<std::pair<std::string, std::string>> const& query);

	/// Builds the Azure StringToSign for SharedKey authorization.
	std::string string_to_sign(std::string const& verb, std::map<std::string, std::string> const& headers,
		std::string const& account, std::string const& path,
		std::vector<std::pair<std::string, std::string>> const& query);

	/// Returns the full value for the Authorization header, i.e. "SharedKey account:signature".
	std::string sign(credentials const& cred, std::string const& verb,
		std::map<std::string, std::string> const& headers, std::string const& path,
		std::vector<std::pair<std::string, std::string>> const& query);

	struct blob_entry {
		std::string name;
		int64_t size{};
		std::string last_modified;
		std::string content_type;
		bool is_prefix{};
	};

	struct list_blobs_result {
		std::vector<blob_entry> blobs;
		std::string next_marker;
	};

	struct container_entry {
		std::string name;
		std::string last_modified;
	};

	/// Parses a List Blobs response (comp=list, restype=container).
	bool parse_list_blobs(std::string const& xml, list_blobs_result & out);

	/// Parses a List Containers response.
	bool parse_list_containers(std::string const& xml, std::vector<container_entry> & out,
		std::string & next_marker);

	/// Parses an Azure error document.
	bool parse_error(std::string const& xml, std::string & code, std::string & message);

	/// Formats a unix timestamp as RFC 1123 in GMT, e.g. "Mon, 03 Nov 2025 00:00:00 GMT".
	std::string rfc1123_date(int64_t unix_time);
	std::string rfc1123_now();

	/// Percent-encodes a blob name, keeping '/' as path separator.
	std::string encode_blob_name(std::string const& name);

	/// Joins a directory prefix and a blob name with exactly one '/'.
	std::string join_name(std::string const& prefix, std::string const& name);
}

#endif
