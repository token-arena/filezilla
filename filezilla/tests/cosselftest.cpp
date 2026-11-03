#include "../src/engine/cos/cossign.h"

#include <cstdio>
#include <string>

// Standalone self-test for the Tencent Cloud COS logic layer (q-sign-algorithm
// signatures and XML parsing). Wired into `make check` as cosselftest.
//
// Signature vectors were produced by an independent Python implementation of the
// documented algorithm and are cross-checked here.

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

char const* const host = "mybucket-1250000000.cos.ap-beijing.myqcloud.com";
std::string const key_time = "1762128000;1762131600";

tcos::credentials test_credentials()
{
	tcos::credentials c;
	c.secret_id = "AKIDexampleSecretId1234567890";
	c.secret_key = "COSsecretKeyForTesting1234567890abcd";
	return c;
}

std::vector<std::pair<std::string, std::string>> host_header()
{
	return {{"Host", host}};
}

void test_put_object_signature()
{
	std::vector<std::pair<std::string, std::string>> headers = {
		{"Host", host}, {"Content-Type", "text/plain"}, {"Content-Length", "13"}};

	auto const s = tcos::sign(test_credentials(), "PUT", "/dir/file.txt", {}, headers, key_time);

	check_eq("put header list", s.header_list, "content-length;content-type;host");
	check_eq("put param list", s.param_list, "");
	check_eq("put http string", s.http_string,
		std::string("put\n/dir/file.txt\n\ncontent-length=13&content-type=text%2Fplain&host=") + host + "\n");
	check_eq("put string to sign", s.string_to_sign,
		"sha1\n1762128000;1762131600\n3525c2f163fe4c76f4032a661d48db5ffd798441\n");
	check_eq("put signature", s.signature, "981082be004d44726b8bfbe59c7821736c789d9c");
	check_eq("put authorization", s.authorization,
		"q-sign-algorithm=sha1&q-ak=AKIDexampleSecretId1234567890"
		"&q-sign-time=1762128000;1762131600&q-key-time=1762128000;1762131600"
		"&q-header-list=content-length;content-type;host&q-url-param-list="
		"&q-signature=981082be004d44726b8bfbe59c7821736c789d9c");
}

void test_get_and_delete_signatures()
{
	auto const g = tcos::sign(test_credentials(), "GET", "/dir/file.txt", {}, host_header(), key_time);
	check_eq("get signature", g.signature, "4f1b3c8e40ff6de5fb1558c4b707cc13f6b0765a");

	auto const d = tcos::sign(test_credentials(), "DELETE", "/dir/file.txt", {}, host_header(), key_time);
	check_eq("delete signature", d.signature, "3be84fe783e3f6578f16bdb08651505cb5a56da9");
}

void test_list_objects_signature()
{
	std::vector<std::pair<std::string, std::string>> params = {
		{"prefix", "dir/"}, {"delimiter", "/"}, {"max-keys", "1000"}};

	auto const s = tcos::sign(test_credentials(), "GET", "/", params, host_header(), key_time);

	check_eq("list param list", s.param_list, "delimiter;max-keys;prefix");
	check_eq("list http string", s.http_string,
		std::string("get\n/\ndelimiter=%2F&max-keys=1000&prefix=dir%2F\nhost=") + host + "\n");
	check_eq("list signature", s.signature, "b216e92e5457d051c5903d80fdec6915c2e96f4f");
}

void test_multipart_signatures()
{
	auto const i = tcos::sign(test_credentials(), "POST", "/big.bin", {{"uploads", ""}}, host_header(), key_time);
	check_eq("initiate param list", i.param_list, "uploads");
	check_eq("initiate signature", i.signature, "0c0554096300871298992823516ccfedc7d07c07");

	std::vector<std::pair<std::string, std::string>> part_params = {
		{"partNumber", "1"}, {"uploadId", "UPLOADID123"}};
	auto const p = tcos::sign(test_credentials(), "PUT", "/big.bin", part_params, host_header(), key_time);
	check_eq("part param list lowercased", p.param_list, "partnumber;uploadid");
	check_eq("part signature", p.signature, "5ef05dbdb46589784b84f365dafb2eaa56ff6d2b");
}

void test_copy_signature()
{
	std::vector<std::pair<std::string, std::string>> headers = {
		{"Host", host}, {"x-cos-copy-source", std::string(host) + "/src.txt"}};

	auto const s = tcos::sign(test_credentials(), "PUT", "/dst.txt", {}, headers, key_time);
	check_eq("copy header list", s.header_list, "host;x-cos-copy-source");
	check_eq("copy signature", s.signature, "359300c6b8b9bf299f5a4229968a58dbfb6bbf1c");
}

void test_helpers()
{
	check_eq("sha1 of abc", tcos::sha1_hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
	check_eq("encode slash", tcos::encode_value("dir/"), "dir%2F");
	check_eq("encode space", tcos::encode_value("a b"), "a%20b");
	check_eq("key time", tcos::make_key_time(1000, 60), "1000;1060");
	check_eq("encode keeps slash in key", tcos::encode_key("dir/a b.txt"), "dir/a%20b.txt");
	check_eq("join", tcos::join_name("docs/", "a.txt"), "docs/a.txt");
}

void test_parse_list_objects()
{
	std::string const xml =
		"<ListBucketResult><Name>" + std::string(host) + "</Name><Prefix>dir/</Prefix><Delimiter>/</Delimiter>"
		"<IsTruncated>true</IsTruncated><NextMarker>dir/sub/</NextMarker>"
		"<Contents><Key>dir/a.txt</Key><Size>11</Size>"
		"<LastModified>2025-11-03T00:00:00.000Z</LastModified><ETag>\"ABC\"</ETag></Contents>"
		"<CommonPrefixes><Prefix>dir/sub/</Prefix></CommonPrefixes>"
		"</ListBucketResult>";

	tcos::list_result res;
	check_true("list parses", tcos::parse_list_objects(xml, res));
	check_eq("prefix", res.prefix, "dir/");
	check_true("truncated", res.truncated);
	check_eq("next marker", res.next_marker, "dir/sub/");
	check_true("two entries", res.entries.size() == 2);
	if (res.entries.size() == 2) {
		check_eq("entry 0 key", res.entries[0].key, "dir/a.txt");
		check_eq_int("entry 0 size", res.entries[0].size, 11);
		check_true("entry 1 is prefix", res.entries[1].is_prefix);
	}

	tcos::list_result bad;
	check_true("garbage fails", !tcos::parse_list_objects("nope", bad));
}

void test_multipart_xml()
{
	std::string code, message;
	check_true("error parses", tcos::parse_error(
		"<Error><Code>NoSuchKey</Code><Message>The specified key does not exist.</Message></Error>",
		code, message));
	check_eq("error code", code, "NoSuchKey");

	std::string upload_id;
	check_true("initiate parses", tcos::parse_initiate_multipart(
		"<InitiateMultipartUploadResult><Bucket>b</Bucket><Key>k</Key>"
		"<UploadId>UPLOAD123</UploadId></InitiateMultipartUploadResult>", upload_id));
	check_eq("upload id", upload_id, "UPLOAD123");

	std::vector<tcos::part_entry> parts = {{1, "\"e1\""}};
	check_eq("complete body", tcos::build_complete_multipart(parts),
		"<CompleteMultipartUpload>"
		"<Part><PartNumber>1</PartNumber><ETag>\"e1\"</ETag></Part>"
		"</CompleteMultipartUpload>");
}

}


void test_header_filtering()
{
	// Mirrors the COS SDK: only whitelisted headers and x-cos*/x-ci* are signed
	check_true("host signed", tcos::should_sign_header("Host"));
	check_true("content-type signed", tcos::should_sign_header("Content-Type"));
	check_true("range signed", tcos::should_sign_header("range"));
	check_true("x-cos signed", tcos::should_sign_header("x-cos-copy-source"));
	check_true("x-ci signed", tcos::should_sign_header("x-ci-upload"));
	check_true("user-agent not signed", !tcos::should_sign_header("User-Agent"));
	check_true("authorization not signed", !tcos::should_sign_header("Authorization"));

	// A header outside the whitelist must not change the signature
	std::vector<std::pair<std::string, std::string>> with_ua = {
		{"Host", host}, {"User-Agent", "FileZilla/3.71.1"}};
	auto const a = tcos::sign(test_credentials(), "GET", "/a.txt", {}, with_ua, key_time);
	auto const b = tcos::sign(test_credentials(), "GET", "/a.txt", {}, host_header(), key_time);
	check_eq("user-agent is ignored", a.signature, b.signature);
	check_eq("user-agent not in header list", a.header_list, "host");
}
int main()
{
	test_put_object_signature();
	test_get_and_delete_signatures();
	test_list_objects_signature();
	test_multipart_signatures();
	test_copy_signature();
	test_helpers();
	test_header_filtering();
	test_parse_list_objects();
	test_multipart_xml();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures) {
		printf("RESULT: FAILED\n");
		return 1;
	}
	printf("RESULT: OK\n");
	return 0;
}