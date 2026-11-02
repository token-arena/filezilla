#include "../src/engine/gcs/gcsjson.h"

#include <cstdio>
#include <string>

// Standalone self-test for the Google Cloud Storage logic layer (JSON API).
// Wired into `make check` as gcsselftest.

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
		printf("FAIL %s\n  got: %lld expected: %lld\n", what,
			static_cast<long long>(got), static_cast<long long>(expected));
	}
}

std::string const list_json = R"({
  "kind": "storage#objects",
  "items": [
    {"kind": "storage#object", "name": "photos/a.jpg", "size": "1024",
     "updated": "2025-11-03T00:00:00.000Z", "contentType": "image/jpeg"},
    {"kind": "storage#object", "name": "photos/b.txt", "size": "0"}
  ],
  "prefixes": ["photos/sub/"],
  "nextPageToken": "token-abc"
})";

void test_list_objects()
{
	gcs::list_objects_result res;
	std::string error;
	check_true("list parses", gcs::parse_list_objects(list_json, res, error));
	check_true("three entries (2 objects + 1 prefix)", res.objects.size() == 3);
	if (res.objects.size() == 3) {
		check_eq("object 0 name", res.objects[0].name, "photos/a.jpg");
		check_eq_int("object 0 size", res.objects[0].size, 1024);
		check_eq("object 0 content type", res.objects[0].content_type, "image/jpeg");
		check_eq("object 0 updated", res.objects[0].updated, "2025-11-03T00:00:00.000Z");
		check_true("object 0 not prefix", !res.objects[0].is_prefix);
		check_eq("prefix name", res.objects[2].name, "photos/sub/");
		check_true("entry 2 is prefix", res.objects[2].is_prefix);
	}
	check_eq("next page token", res.next_page_token, "token-abc");

	gcs::list_objects_result empty;
	std::string err2;
	check_true("empty listing parses", gcs::parse_list_objects(R"({"kind":"storage#objects"})", empty, err2));
	check_true("no entries", empty.objects.empty());
	check_true("no token", empty.next_page_token.empty());

	gcs::list_objects_result bad;
	std::string err3;
	check_true("garbage fails", !gcs::parse_list_objects("not json", bad, err3));
}

void test_list_buckets()
{
	std::vector<gcs::bucket_entry> buckets;
	std::string error;
	check_true("buckets parse", gcs::parse_list_buckets(
		R"({"items":[{"name":"b1","location":"EU"},{"name":"b2"}]})", buckets, error));
	check_true("two buckets", buckets.size() == 2);
	if (buckets.size() == 2) {
		check_eq("bucket 0", buckets[0].name, "b1");
		check_eq("bucket 0 location", buckets[0].location, "EU");
	}
}

void test_parse_error()
{
	std::string message;
	check_true("error parses", gcs::parse_error(
		R"({"error":{"code":404,"message":"No such object: b/o"}})", message));
	check_eq("error message", message, "No such object: b/o");

	std::string m2;
	check_true("plain object is not an error", !gcs::parse_error(R"({"kind":"storage#object"})", m2));
}

void test_encoding()
{
	check_eq("encode slash", gcs::encode_object_name("a/b.txt"), "a%2Fb.txt");
	check_eq("encode space", gcs::encode_object_name("my file.txt"), "my%20file.txt");
	check_eq("encode utf8", gcs::encode_object_name("\xe7\x9b\xae\xe5\xbd\x95"), "%E7%9B%AE%E5%BD%95");
	check_eq("encode keeps unreserved", gcs::encode_object_name("a-b_c.d~e"), "a-b_c.d~e");
	check_eq("join", gcs::join_name("docs/", "a.txt"), "docs/a.txt");
	check_eq("join no slash", gcs::join_name("docs", "a.txt"), "docs/a.txt");
}

void test_urls()
{
	check_eq("list url", gcs::list_objects_url("mybucket", "photos/", true, "tok"),
		"https://storage.googleapis.com/storage/v1/b/mybucket/o?maxResults=1000"
		"&prefix=photos%2F&delimiter=%2F&pageToken=tok");

	check_eq("list url without prefix", gcs::list_objects_url("b", "", false, ""),
		"https://storage.googleapis.com/storage/v1/b/b/o?maxResults=1000");

	check_eq("object media url", gcs::object_url("b", "dir/a b.txt"),
		"https://storage.googleapis.com/storage/v1/b/b/o/dir%2Fa%20b.txt?alt=media");

	check_eq("object json url", gcs::object_json_url("b", "a.txt"),
		"https://storage.googleapis.com/storage/v1/b/b/o/a.txt");

	check_eq("upload url", gcs::upload_url("b", "dir/a.txt"),
		"https://storage.googleapis.com/upload/storage/v1/b/b/o?uploadType=media&name=dir%2Fa.txt");
}

}

int main()
{
	test_list_objects();
	test_list_buckets();
	test_parse_error();
	test_encoding();
	test_urls();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures) {
		printf("RESULT: FAILED\n");
		return 1;
	}
	printf("RESULT: OK\n");
	return 0;
}
