#ifndef FILEZILLA_ENGINE_S3_SIGNER_HEADER
#define FILEZILLA_ENGINE_S3_SIGNER_HEADER

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Pure logic for AWS Signature Version 4, kept free of engine dependencies so
// it can be unit-tested standalone (see tests/s3selftest.cpp).
namespace s3 {

	/// RFC 3986 percent encoding as required by SigV4.
	/// Unreserved characters are A-Z a-z 0-9 - . _ ~ ; '/' is only kept if keep_slash is set.
	std::string uri_encode(std::string_view const& in, bool keep_slash = false);

	/// Builds the canonical query string: parameters are URI-encoded, sorted by
	/// encoded name (then encoded value) and joined with '&'.
	std::string canonical_query_string(std::vector<std::pair<std::string, std::string>> const& params);

	struct credentials {
		std::string access_key;
		std::string secret_key;
		std::string session_token; // optional
	};

	struct signable_request {
		std::string method;
		std::string host;                     // value of the Host header
		std::string canonical_uri = "/";      // already percent-encoded, starts with '/'
		std::vector<std::pair<std::string, std::string>> query; // decoded names/values
		std::map<std::string, std::string> headers; // any extra headers to sign, names in lowercase
		std::string payload_hash;             // lowercase hex sha256 of the body, or "UNSIGNED-PAYLOAD"
	};

	struct signing_result {
		std::string authorization;      // value for the Authorization header
		std::string amz_date;           // value for the x-amz-date header
		std::string canonical_request;  // useful for debugging and tests
		std::string string_to_sign;
		std::string signature;          // lowercase hex
		std::string signed_headers;     // ';' separated, lowercase
	};

	/// Returns the lowercase hex SHA-256 of the given data.
	std::string sha256_hex(std::string_view const& data);

	/// Returns the lowercase hex HMAC-SHA256 of data with the given key.
	std::string hmac_sha256_hex(std::string_view const& key, std::string_view const& data);

	/// Formats the given time as the ISO8601 "YYYYMMDDTHHMMSSZ" string SigV4 uses.
	std::string format_amz_date(int64_t unix_time);

	/// Current time as "YYYYMMDDTHHMMSSZ".
	std::string amz_date_now();

	/// Computes the SigV4 signature. amz_date must be "YYYYMMDDTHHMMSSZ".
	/// On return, req.host/req.headers are not modified; the caller is expected to
	/// send the returned amz_date plus x-amz-content-sha256 and, for temporary
	/// credentials, x-amz-security-token.
	signing_result sign(signable_request const& req, credentials const& cred,
		std::string const& region, std::string const& service, std::string const& amz_date);
}

#endif
