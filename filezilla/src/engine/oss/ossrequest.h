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
