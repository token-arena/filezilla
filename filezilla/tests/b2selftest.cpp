#include "../src/engine/b2/b2json.h"

#include <libfilezilla/json.hpp>

#include <cstdio>
#include <string>

// Standalone self-test for the Backblaze B2 protocol logic layer (JSON parsing,
// request building, name mapping). Wired into `make check` as b2selftest.

namespace {

int g_failures = 0;
int g_checks = 0;

void check_eq(char const* what, std::string const& got, std::string const& expected)
{
	++g_checks;
	if (got != expected) {
		++g_failures;
		printf("FAIL %s\n  got:      %s\n  expected: %s\n", what, got.c_str(), expected.c_str());
	}
}

void check_true(char const* what, bool cond)
{
	++g_checks;
	if (!cond) {
		++g_failures;
		printf("FAIL %s\n", what);
	}
}

void check_eq_int(char const* what, int64_t got, int64_t expected)
{
	++g_checks;
	if (got != expected) {
		++g_failures;
		printf("FAIL %s\n  got: %lld expected: %lld\n", what, static_cast<long long>(got), static_cast<long long>(expected));
	}
}

std::string const auth_response = R"({
  "accountId": "abc123",
  "authorizationToken": "4_00aaexampletoken",
  "apiInfo": {
    "storageApi": {
      "apiUrl": "https://api001.backblazeb2.com",
      "downloadUrl": "https://f001.backblazeb2.com",
      "capabilities": ["listFiles", "readFiles", "writeFiles"],
      "bucketId": "deadbeef",
      "bucketName": "my-bucket",
      "namePrefix": null
    }
  }
})";

void test_parse_authorize()
{
	b2::auth_info info;
	std::string error;
	check_true("authorize parses", b2::parse_authorize(auth_response, info, error));
	check_eq("account id", info.account_id, "abc123");
	check_eq("token", info.authorization_token, "4_00aaexampletoken");
	check_eq("api url", info.api_url, "https://api001.backblazeb2.com");
	check_eq("download url", info.download_url, "https://f001.backblazeb2.com");
	check_eq("bucket id", info.bucket_id, "deadbeef");
	check_eq("bucket name", info.bucket_name, "my-bucket");

	b2::auth_info bad;
	std::string err2;
	check_true("authorize garbage fails", !b2::parse_authorize("not json", bad, err2));
	check_true("authorize error message set", !err2.empty());

	// An error document must be reported with code and message
	std::string err3;
	check_true("authorize error doc fails",
		!b2::parse_authorize(R"({"status":401,"code":"unauthorized","message":"bad key"})", bad, err3));
	check_true("authorize error contains code", err3.find("unauthorized") != std::string::npos);
}

std::string const list_response = R"({
  "files": [
    {"fileId": "id1", "fileName": "photos/a.jpg", "contentLength": 1234, "uploadTimestamp": 1600000000000, "action": "upload"},
    {"fileId": "id2", "fileName": "photos/sub/", "contentLength": 0, "uploadTimestamp": 1600000001000, "action": "folder"}
  ],
  "nextFileName": "photos/sub/",
  "bucketId": "deadbeef"
})";

void test_parse_list()
{
	b2::list_result res;
	std::string error;
	check_true("list parses", b2::parse_list_file_names(list_response, res, error));
	check_true("list has two entries", res.files.size() == 2);
	if (res.files.size() == 2) {
		check_eq("entry 0 name", res.files[0].file_name, "photos/a.jpg");
		check_eq("entry 0 id", res.files[0].file_id, "id1");
		check_eq_int("entry 0 size", res.files[0].size, 1234);
		check_eq_int("entry 0 timestamp", res.files[0].upload_timestamp, 1600000000000LL);
		check_eq("entry 1 action", res.files[1].action, "folder");
	}
	check_eq("next file name", res.next_file_name, "photos/sub/");
	check_true("has more", res.has_more);

	b2::list_result empty;
	std::string err2;
	check_true("empty list parses", b2::parse_list_file_names(R"({"files":[]})", empty, err2));
	check_true("empty list has no entries", empty.files.empty());
	check_true("empty list has no continuation", !empty.has_more);
}

void test_parse_buckets()
{
	std::vector<b2::bucket_entry> buckets;
	std::string error;
	check_true("buckets parse", b2::parse_list_buckets(
		R"({"buckets":[{"bucketId":"a1","bucketName":"first"},{"bucketId":"b2","bucketName":"second"}]})",
		buckets, error));
	check_true("two buckets", buckets.size() == 2);
	if (buckets.size() == 2) {
		check_eq("bucket 0 name", buckets[0].bucket_name, "first");
		check_eq("bucket 1 id", buckets[1].bucket_id, "b2");
	}
}

void test_request_bodies()
{
	auto const list = b2::make_list_request("bid", "photos/", "/", 1000, "photos/x");
	auto doc = fz::json::parse(list);
	check_eq("list bucketId", doc["bucketId"].string_value(), "bid");
	check_eq("list prefix", doc["prefix"].string_value(), "photos/");
	check_eq("list delimiter", doc["delimiter"].string_value(), "/");
	check_eq_int("list maxFileCount", doc["maxFileCount"].number_value<int64_t>(0), 1000);
	check_eq("list startFileName", doc["startFileName"].string_value(), "photos/x");

	auto const del = fz::json::parse(b2::make_delete_file_request("a/b.txt", "fid"));
	check_eq("delete fileName", del["fileName"].string_value(), "a/b.txt");
	check_eq("delete fileId", del["fileId"].string_value(), "fid");

	auto const copy = fz::json::parse(b2::make_copy_request("srcid", "new/name.txt", "bid"));
	check_eq("copy source", copy["sourceFileId"].string_value(), "srcid");
	check_eq("copy dest", copy["fileName"].string_value(), "new/name.txt");
	check_eq("copy bucket", copy["destinationBucketId"].string_value(), "bid");

	auto const upload = fz::json::parse(b2::make_upload_url_request("bid"));
	check_eq("upload url bucketId", upload["bucketId"].string_value(), "bid");
}

void test_encode_file_name()
{
	check_eq("encode plain", b2::encode_file_name("photos/a.jpg"), "photos/a.jpg");
	check_eq("encode space", b2::encode_file_name("my file.txt"), "my%20file.txt");
	check_eq("encode utf8", b2::encode_file_name("\xe7\x9b\xae\xe5\xbd\x95.txt"), "%E7%9B%AE%E5%BD%95.txt");
	check_eq("encode plus", b2::encode_file_name("a+b"), "a%2Bb");
	check_eq("encode keeps slash", b2::encode_file_name("a/b/c"), "a/b/c");
}

void test_join_name()
{
	check_eq("join empty prefix", b2::join_name("", "file.txt"), "file.txt");
	check_eq("join simple", b2::join_name("docs/", "file.txt"), "docs/file.txt");
	check_eq("join without slash", b2::join_name("docs", "file.txt"), "docs/file.txt");
	check_eq("join absolute name", b2::join_name("docs/", "/other.txt"), "docs//other.txt");
}

}

int main()
{
	test_parse_authorize();
	test_parse_list();
	test_parse_buckets();
	test_request_bodies();
	test_encode_file_name();
	test_join_name();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures) {
		printf("RESULT: FAILED\n");
		return 1;
	}
	printf("RESULT: OK\n");
	return 0;
}
