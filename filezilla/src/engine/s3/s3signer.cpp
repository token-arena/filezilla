#include "s3signer.h"

#include <libfilezilla/encode.hpp>
#include <libfilezilla/hash.hpp>
#include <libfilezilla/time.hpp>

#include <algorithm>
#include <cstdio>

namespace s3 {

namespace {

std::string const& lowercase(std::string & s)
{
	for (auto & c : s) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return s;
}

std::string trimmed(std::string_view const& in)
{
	size_t start = 0;
	while (start < in.size() && (in[start] == ' ' || in[start] == '\t')) {
		++start;
	}
	size_t end = in.size();
	while (end > start && (in[end - 1] == ' ' || in[end - 1] == '\t')) {
		--end;
	}
	return std::string(in.substr(start, end - start));
}

}

std::string uri_encode(std::string_view const& in, bool keep_slash)
{
	static char const hex[] = "0123456789ABCDEF";

	std::string out;
	out.reserve(in.size() + in.size() / 2);

	for (unsigned char const c : in) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '.' || c == '_' || c == '~')
		{
			out.push_back(static_cast<char>(c));
		}
		else if (c == '/' && keep_slash) {
			out.push_back('/');
		}
		else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0x0f]);
		}
	}

	return out;
}

std::string canonical_query_string(std::vector<std::pair<std::string, std::string>> const& params)
{
	std::vector<std::pair<std::string, std::string>> encoded;
	encoded.reserve(params.size());
	for (auto const& p : params) {
		encoded.emplace_back(uri_encode(p.first), uri_encode(p.second));
	}

	std::sort(encoded.begin(), encoded.end());

	std::string ret;
	for (auto const& p : encoded) {
		if (!ret.empty()) {
			ret += '&';
		}
		ret += p.first;
		ret += '=';
		ret += p.second;
	}

	return ret;
}

std::string sha256_hex(std::string_view const& data)
{
	auto const digest = fz::sha256(data);
	return fz::hex_encode<std::string>(digest);
}

std::string hmac_sha256_hex(std::string_view const& key, std::string_view const& data)
{
	auto const mac = fz::hmac_sha256(key, data);
	return fz::hex_encode<std::string>(mac);
}

std::string format_amz_date(int64_t unix_time)
{
	// Handle negative times defensively; the epoch itself is never used in practice.
	uint64_t const t = unix_time < 0 ? 0 : static_cast<uint64_t>(unix_time);

	uint64_t const days_total = t / 86400;
	uint64_t const secs_of_day = t % 86400;

	// Convert days since epoch to civil date (Howard Hinnant's algorithm).
	int64_t z = static_cast<int64_t>(days_total) + 719468;
	int64_t const era = (z >= 0 ? z : z - 146096) / 146097;
	uint64_t const doe = static_cast<uint64_t>(z - era * 146097);
	uint64_t const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t const y = static_cast<int64_t>(yoe) + era * 400;
	uint64_t const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint64_t const mp = (5 * doy + 2) / 153;
	uint64_t const d = doy - (153 * mp + 2) / 5 + 1;
	uint64_t const m = mp < 10 ? mp + 3 : mp - 9;
	int64_t const year = y + (m <= 2 ? 1 : 0);

	char buf[17];
	snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02dZ",
		static_cast<int>(year), static_cast<int>(m), static_cast<int>(d),
		static_cast<int>(secs_of_day / 3600),
		static_cast<int>((secs_of_day / 60) % 60),
		static_cast<int>(secs_of_day % 60));
	return std::string(buf, 16);
}

std::string amz_date_now()
{
	return format_amz_date(fz::datetime::now().get_time_t());
}

signing_result sign(signable_request const& req, credentials const& cred,
	std::string const& region, std::string const& service, std::string const& amz_date)
{
	signing_result res;
	res.amz_date = amz_date;

	std::string const date = amz_date.substr(0, 8);

	// Collect the headers to sign: caller supplied ones plus the mandatory trio.
	std::map<std::string, std::string> headers;
	for (auto const& h : req.headers) {
		std::string name = h.first;
		headers[lowercase(name)] = trimmed(h.second);
	}
	headers["host"] = req.host;
	headers["x-amz-content-sha256"] = req.payload_hash;
	headers["x-amz-date"] = amz_date;
	if (!cred.session_token.empty() && !headers.count("x-amz-security-token")) {
		headers["x-amz-security-token"] = cred.session_token;
	}

	std::string canonical_headers;
	for (auto const& h : headers) {
		canonical_headers += h.first;
		canonical_headers += ':';
		canonical_headers += h.second;
		canonical_headers += '\n';
		if (!res.signed_headers.empty()) {
			res.signed_headers += ';';
		}
		res.signed_headers += h.first;
	}

	std::string const canonical_uri = req.canonical_uri.empty() ? std::string("/") : req.canonical_uri;

	res.canonical_request = req.method;
	res.canonical_request += '\n';
	res.canonical_request += canonical_uri;
	res.canonical_request += '\n';
	res.canonical_request += canonical_query_string(req.query);
	res.canonical_request += '\n';
	res.canonical_request += canonical_headers;
	res.canonical_request += '\n';
	res.canonical_request += res.signed_headers;
	res.canonical_request += '\n';
	res.canonical_request += req.payload_hash;

	std::string const scope = date + '/' + region + '/' + service + "/aws4_request";

	res.string_to_sign = "AWS4-HMAC-SHA256\n";
	res.string_to_sign += amz_date;
	res.string_to_sign += '\n';
	res.string_to_sign += scope;
	res.string_to_sign += '\n';
	res.string_to_sign += sha256_hex(res.canonical_request);

	// Derive the signing key: kDate -> kRegion -> kService -> kSigning
	auto key = fz::hmac_sha256("AWS4" + cred.secret_key, date);
	key = fz::hmac_sha256(key, region);
	key = fz::hmac_sha256(key, service);
	key = fz::hmac_sha256(key, std::string_view("aws4_request"));

	res.signature = fz::hex_encode<std::string>(fz::hmac_sha256(key, res.string_to_sign));

	res.authorization = "AWS4-HMAC-SHA256 Credential=";
	res.authorization += cred.access_key;
	res.authorization += '/';
	res.authorization += scope;
	res.authorization += ", SignedHeaders=";
	res.authorization += res.signed_headers;
	res.authorization += ", Signature=";
	res.authorization += res.signature;

	return res;
}

}
