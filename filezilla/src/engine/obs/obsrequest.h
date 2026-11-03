#ifndef FILEZILLA_ENGINE_OBS_OBSREQUEST_HEADER
#define FILEZILLA_ENGINE_OBS_OBSREQUEST_HEADER

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pure logic for the Huawei Cloud OBS REST API. OBS uses an S3-V2 style header
// signature: base64(HMAC-SHA1(secret, StringToSign)) sent as
// "Authorization: OBS <AccessKeyId>:<Signature>".
// No engine dependencies, so it can be unit-tested standalone
// (see tests/obsselftest.cpp).
namespace obs {

	struct credentials {
		std::string access_key;
		std::string secret_key;
		std::string security_token; // optional, x-obs-security-token
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
		bool truncated{};
		std::vector<object_entry> entries;
	};

	struct bucket_entry {
		std::string name;
		std::string creation_date;
	};

	struct part_entry {
		int number{};
		std::string etag;
	};

	/// RFC 1123 timestamp in GMT for the Date header.
	std::string rfc1123_date(int64_t unix_time);
	std::string rfc1123_now();

	/// Lowercased x-obs-* headers, trimmed, sorted, "name:value\n" each.
	std::string canonicalized_obs_headers(std::map<std::string, std::string> const& headers);

	/// "/bucket/key" plus "?sub1&sub2=value" for signed sub-resources
	/// (uploads, uploadId, partNumber, ...), sorted by name.
	std::string canonicalized_resource(std::string const& bucket, std::string const& key,
		std::vector<std::pair<std::string, std::string>> const& subresources);

	std::string string_to_sign(std::string const& verb, std::map<std::string, std::string> const& headers,
		std::string const& bucket, std::string const& key,
		std::vector<std::pair<std::string, std::string>> const& subresources);

	/// Full Authorization header value, i.e. "OBS <access key>:<signature>".
	std::string sign(credentials const& cred, std::string const& verb,
		std::map<std::string, std::string> const& headers, std::string const& bucket,
		std::string const& key, std::vector<std::pair<std::string, std::string>> const& subresources);

	/// Percent-encodes an object key for the request path, keeping '/'.
	std::string encode_key(std::string const& key);

	/// Joins a directory prefix and a name with exactly one '/'.
	std::string join_name(std::string const& prefix, std::string const& name);

	// --- XML ---

	bool parse_list_buckets(std::string const& xml, std::vector<bucket_entry> & out);
	bool parse_list_objects(std::string const& xml, list_result & out);
	bool parse_error(std::string const& xml, std::string & code, std::string & message);
	bool parse_initiate_multipart(std::string const& xml, std::string & upload_id);
	std::string build_complete_multipart(std::vector<part_entry> const& parts);
}

#endif
