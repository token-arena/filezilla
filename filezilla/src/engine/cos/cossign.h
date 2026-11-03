#ifndef FILEZILLA_ENGINE_COS_COSSIGN_HEADER
#define FILEZILLA_ENGINE_COS_COSSIGN_HEADER

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pure logic for the Tencent Cloud COS XML API signature
// (q-sign-algorithm=sha1) plus the XML documents it exchanges.
// No engine dependencies, so it can be unit-tested standalone
// (see tests/cosselftest.cpp).
namespace tcos {

	struct credentials {
		std::string secret_id;
		std::string secret_key;
		std::string security_token; // optional
	};

	/// Lowercase hex SHA1 of the input.
	std::string sha1_hex(std::string_view const& data);

	/// Lowercase hex HMAC-SHA1 of the input.
	std::string hmac_sha1_hex(std::string_view const& key, std::string_view const& data);

	/// URL-encodes a value for the header/parameter lists of the signature.
	std::string encode_value(std::string const& value);

	/// "start;end" in unix seconds, one hour apart.
	std::string make_key_time(int64_t now, int64_t lifetime_seconds = 3600);
	std::string key_time_now();

	struct signature {
		std::string param_list;    // q-url-param-list
		std::string header_list;   // q-header-list
		std::string http_string;
		std::string string_to_sign;
		std::string signature;
		std::string key_time;
		std::string authorization; // full Authorization header value
	};

	/// Builds the COS signature. \param headers must contain "host"; header and
	/// parameter names are treated case insensitively.
	signature sign(credentials const& cred, std::string const& method, std::string const& pathname,
		std::vector<std::pair<std::string, std::string>> const& params,
		std::vector<std::pair<std::string, std::string>> const& headers,
		std::string const& key_time);

	// --- XML documents ---

	struct object_entry {
		std::string key;
		int64_t size{};
		std::string last_modified;
		std::string etag;
		bool is_prefix{};
	};

	struct list_result {
		std::string name;
		std::string prefix;
		std::string delimiter;
		std::string next_marker;
		bool truncated{};
		std::vector<object_entry> entries;
	};

	struct part_entry {
		int number{};
		std::string etag;
	};

	bool parse_list_objects(std::string const& xml, list_result & out);
	bool parse_error(std::string const& xml, std::string & code, std::string & message);
	bool parse_initiate_multipart(std::string const& xml, std::string & upload_id);
	std::string build_complete_multipart(std::vector<part_entry> const& parts);

	/// Percent-encodes an object key for the request path, keeping '/'.
	std::string encode_key(std::string const& key);

	/// Joins a directory prefix and a name with exactly one '/'.
	std::string join_name(std::string const& prefix, std::string const& name);
}

#endif
