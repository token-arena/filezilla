#ifndef FILEZILLA_ENGINE_OSS_OSSREQUEST_HEADER
#define FILEZILLA_ENGINE_OSS_OSSREQUEST_HEADER

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pure logic for the Alibaba Cloud OSS REST API using signature version 1
// (the documented "OSS AccessKeyId:Signature" scheme, HMAC-SHA1 over a
// canonical string). No engine dependencies, so it can be unit-tested
// standalone (see tests/osseselftest.cpp).
namespace oss {

	struct credentials {
		std::string access_key;
		std::string secret_key;
		std::string security_token; // optional STS token
	};

	struct object_entry {
		std::string key;
		int64_t size{};
		std::string last_modified;
		std::string etag;
		std::string storage_class;
		bool is_prefix{};
	};

	struct list_result {
		std::string bucket;
		std::string prefix;
		std::string delimiter;
		std::string next_marker;
		int64_t max_keys{};
		bool truncated{};
		std::vector<object_entry> entries;
	};

	struct bucket_entry {
		std::string name;
		std::string location;
		std::string creation_date;
	};

	struct part_entry {
		int number{};
		std::string etag;
	};

	/// RFC 1123 timestamp in GMT as required by the Date header.
	std::string rfc1123_date(int64_t unix_time);
	std::string rfc1123_now();

	/// Lowercased x-oss-* headers, trimmed, sorted by name, "name:value\n" each.
	std::string canonicalized_oss_headers(std::map<std::string, std::string> const& headers);

	/// "/bucket/key" plus a "\nname" or "\nname=value" line per signed sub-resource,
	/// sorted by name. Only true OSS sub-resources belong here (uploads, uploadId,
	/// partNumber, acl, ...), not regular query parameters such as prefix.
	std::string canonicalized_resource(std::string const& bucket, std::string const& key,
		std::vector<std::pair<std::string, std::string>> const& subresources);

	std::string string_to_sign(std::string const& verb, std::map<std::string, std::string> const& headers,
		std::string const& bucket, std::string const& key,
		std::vector<std::pair<std::string, std::string>> const& subresources);

	/// Full value for the Authorization header, i.e. "OSS <access key>:<signature>".
	std::string sign(credentials const& cred, std::string const& verb,
		std::map<std::string, std::string> const& headers, std::string const& bucket,
		std::string const& key, std::vector<std::pair<std::string, std::string>> const& subresources);

	// --- signature version 4 (OSS4-HMAC-SHA256, recommended by Alibaba) ---

	enum class signature_version {
		v1,   // "OSS <ak>:<base64(HMAC-SHA1)>"
		v4,   // "OSS4-HMAC-SHA256 Credential=...,Signature=..."
		auto_fallback // v4 first, retry v1 when the service rejects the signature
	};

	/// Parses "v1", "v4" or "auto" (anything else yields v4).
	signature_version parse_signature_version(std::string const& value);

	/// ISO8601 basic timestamp as used by x-oss-date, e.g. "20250411T064124Z".
	std::string iso8601_date(int64_t unix_time);
	std::string iso8601_now();

	/// Derives the signing region from an OSS endpoint such as
	/// "oss-cn-hangzhou.aliyuncs.com" or "bucket.oss-cn-hangzhou.aliyuncs.com".
	/// Returns an empty string when it cannot be determined.
	std::string region_from_endpoint(std::string const& host);

	struct v4_result {
		std::string timestamp;        // x-oss-date value
		std::string scope;
		std::string canonical_request;
		std::string string_to_sign;
		std::string signature;
		std::string authorization;
	};

	/// Builds a V4 signature. \p path is the decoded resource path (no query),
	/// \p query holds the decoded query parameters (empty values are allowed),
	/// \p headers are the headers to sign (x-oss-*, content-type, content-md5 are
	/// signed automatically, everything else must also be listed in
	/// \p additional_headers).
	v4_result sign_v4(credentials const& cred, std::string const& verb, std::string const& path,
		std::vector<std::pair<std::string, std::string>> const& query,
		std::map<std::string, std::string> const& headers,
		std::string const& region, std::string const& timestamp,
		std::vector<std::string> const& additional_headers);

	/// Percent-encodes an object key for the request path, keeping '/'.
	std::string encode_key(std::string const& key);

	/// Joins a directory prefix and a name with exactly one '/'.
	std::string join_name(std::string const& prefix, std::string const& name);

	// --- XML parsing / building ---

	bool parse_list_buckets(std::string const& xml, std::vector<bucket_entry> & out);
	bool parse_list_objects(std::string const& xml, list_result & out);
	bool parse_error(std::string const& xml, std::string & code, std::string & message);
	bool parse_initiate_multipart(std::string const& xml, std::string & upload_id);
	std::string build_complete_multipart(std::vector<part_entry> const& parts);
}

#endif
