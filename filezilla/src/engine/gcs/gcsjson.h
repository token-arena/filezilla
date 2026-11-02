#ifndef FILEZILLA_ENGINE_GCS_GCSJSON_HEADER
#define FILEZILLA_ENGINE_GCS_GCSJSON_HEADER

#include <cstdint>
#include <string>
#include <vector>

// Pure logic for the Google Cloud Storage JSON API (storage/v1).
// The access token is supplied as the password by the user and sent as a Bearer
// token; requests and responses are JSON.
// Kept free of engine dependencies for standalone unit testing
// (see tests/gcsselftest.cpp).
namespace gcs {

	struct object_entry {
		std::string name;
		int64_t size{};
		std::string updated;
		std::string content_type;
		bool is_prefix{};
	};

	struct list_objects_result {
		std::vector<object_entry> objects; // prefixes are appended here with is_prefix set
		std::string next_page_token;
	};

	struct bucket_entry {
		std::string name;
		std::string location;
	};

	/// Parses a storage/v1 object listing (items + prefixes + nextPageToken).
	bool parse_list_objects(std::string const& json, list_objects_result & out, std::string & error);

	/// Parses a storage/v1 bucket listing.
	bool parse_list_buckets(std::string const& json, std::vector<bucket_entry> & out, std::string & error);

	/// Extracts error.message from a Google API error document.
	bool parse_error(std::string const& json, std::string & message);

	/// Percent-encodes an object name for use as a single path segment
	/// (i.e. every '/' is encoded as well).
	std::string encode_object_name(std::string const& name);

	/// Joins a directory prefix and an object name with exactly one '/'.
	std::string join_name(std::string const& prefix, std::string const& name);

	/// Builds a storage/v1 objects listing URL, including the optional page token.
	std::string list_objects_url(std::string const& bucket, std::string const& prefix,
		bool with_delimiter, std::string const& page_token);

	/// Builds the media download URL for an object.
	std::string object_url(std::string const& bucket, std::string const& object);

	/// Builds the JSON API URL for a single object's metadata/operations.
	std::string object_json_url(std::string const& bucket, std::string const& object);

	/// Builds the media upload URL (uploadType=media).
	std::string upload_url(std::string const& bucket, std::string const& object);
}

#endif
