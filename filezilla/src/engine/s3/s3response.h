#ifndef FILEZILLA_ENGINE_S3_S3RESPONSE_HEADER
#define FILEZILLA_ENGINE_S3_S3RESPONSE_HEADER

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// XML response parsing for the S3 REST API.
// Free of engine dependencies so it can be unit-tested standalone.
namespace s3 {

	struct object_entry {
		std::string key;
		int64_t size{};
		std::string etag;
		std::string last_modified; // ISO8601 as returned by the server
		std::string storage_class;
		bool is_prefix{};          // true if this came from <CommonPrefixes>
	};

	struct list_result {
		std::string bucket;
		std::string prefix;
		std::string delimiter;
		std::string continuation_token;
		std::string next_continuation_token; // V2: NextContinuationToken, V1: NextMarker
		int64_t max_keys{};
		bool truncated{};
		std::vector<object_entry> entries;
	};

	struct error_info {
		std::string code;
		std::string message;
		std::string request_id;
		std::string resource;
	};

	struct bucket_entry {
		std::string name;
		std::string creation_date;
	};

	/// Parses a ListBuckets response.
	bool parse_list_buckets(std::string_view const& xml, std::vector<bucket_entry> & out);

	/// Parses a ListObjects(V2) response. Handles both V1 (Marker/NextMarker) and
	/// V2 (ContinuationToken/NextContinuationToken) documents.
	/// Returns false if the document is not well-formed XML.
	bool parse_list_objects(std::string_view const& xml, list_result & out);

	/// Parses an S3 error document. Returns false if the document is not XML.
	bool parse_error(std::string_view const& xml, error_info & out);
}

#endif
