#include "../src/engine/obs/obsrequest.h"

#include <cstdio>
#include <string>

// Standalone self-test for the Huawei Cloud OBS logic layer (header signature
// and XML parsing). Wired into `make check` as obsselftest.
//
// Signature vectors were produced by an independent Python implementation of the
// documented OBS signature algorithm and are cross-checked here.

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

std::string const test_date = "Mon, 03 Nov 2025 00:00:00 GMT";

obs::credentials test_credentials()
{
	obs::credentials c;
	c.access_key = "AKIAexampleAccessKeyId";
	c.secret_key = "exampleSecretAccessKeyForTesting1234567890";
	return c;
}

std::map<std::string, std::string> base_headers()
{
	return {{"Date", test_date}};
}

void test_service_and_bucket_signatures()
{
	check_eq("service string to sign",
		obs::string_to_sign("GET", base_headers(), "", "", {}),
		"GET\n\n\n" + test_date + "\n/");
	check_eq("service signature",
		obs::sign(test_credentials(), "GET", base_headers(), "", "", {}),
		"OBS AKIAexampleAccessKeyId:MnnKavxHJMVZa68BfCbLfQe6p/w=");

	check_eq("list objects signature",
		obs::sign(test_credentials(), "GET", base_headers(), "mybucket", "", {}),
		"OBS AKIAexampleAccessKeyId:ZJgDLrTCWN+KoqNN4zOafT6xWuk=");
}

void test_object_signatures()
{
	auto headers = base_headers();
	headers["Content-Type"] = "text/plain";
	headers["Content-Length"] = "13";

	check_eq("put string to sign",
		obs::string_to_sign("PUT", headers, "mybucket", "dir/file.txt", {}),
		"PUT\n\ntext/plain\n" + test_date + "\n/mybucket/dir/file.txt");
	check_eq("put signature",
		obs::sign(test_credentials(), "PUT", headers, "mybucket", "dir/file.txt", {}),
		"OBS AKIAexampleAccessKeyId:CPK42vYbO8e7o8upwuDYaFECqeQ=");

	check_eq("get signature",
		obs::sign(test_credentials(), "GET", base_headers(), "mybucket", "dir/file.txt", {}),
		"OBS AKIAexampleAccessKeyId:Hss29aumHt5xA875R/S8c+5J6mI=");

	check_eq("delete signature",
		obs::sign(test_credentials(), "DELETE", base_headers(), "mybucket", "dir/file.txt", {}),
		"OBS AKIAexampleAccessKeyId:H/yHEy4Xk1kPh+2xrAFxZvX+PB4=");
}

void test_multipart_signatures()
{
	// Sub-resources are appended as "?a&b=c" (S3 V2 style), not as separate lines
	check_eq("initiate multipart string to sign",
		obs::string_to_sign("POST", base_headers(), "mybucket", "big.bin", {{"uploads", ""}}),
		"POST\n\n\n" + test_date + "\n/mybucket/big.bin?uploads");
	check_eq("initiate multipart signature",
		obs::sign(test_credentials(), "POST", base_headers(), "mybucket", "big.bin", {{"uploads", ""}}),
		"OBS AKIAexampleAccessKeyId:q0uDKoMuIcEFWD/Ulq7Z0QWzMlw=");

	std::vector<std::pair<std::string, std::string>> part_subs = {
		{"partNumber", "1"}, {"uploadId", "0004B9894A22E5B1888A1E29F8236E2D"}};
	check_eq("upload part string to sign",
		obs::string_to_sign("PUT", base_headers(), "mybucket", "big.bin", part_subs),
		"PUT\n\n\n" + test_date +
			"\n/mybucket/big.bin?partnumber=1&uploadid=0004B9894A22E5B1888A1E29F8236E2D");
	check_eq("upload part signature",
		obs::sign(test_credentials(), "PUT", base_headers(), "mybucket", "big.bin", part_subs),
		"OBS AKIAexampleAccessKeyId:p4wWsVa1fWP4fQgMRM5sEkdyBys=");
}

void test_header_canonicalization()
{
	auto headers = base_headers();
	headers["x-obs-copy-source"] = "/mybucket/src.txt";
	check_eq("copy signature",
		obs::sign(test_credentials(), "PUT", headers, "mybucket", "dst.txt", {}),
		"OBS AKIAexampleAccessKeyId:Vn72Z8L1LrQrVfaze6KOKpxZvww=");

	std::map<std::string, std::string> meta = {
		{"x-obs-meta-b", "2"},
		{"X-OBS-META-A", "1"},
	};
	check_eq("canonicalized obs headers", obs::canonicalized_obs_headers(meta),
		"x-obs-meta-a:1\nx-obs-meta-b:2\n");

	auto with_meta = base_headers();
	with_meta["x-obs-meta-b"] = "2";
	with_meta["X-OBS-META-A"] = "1";
	check_eq("meta headers signature",
		obs::sign(test_credentials(), "PUT", with_meta, "mybucket", "a.txt", {}),
		"OBS AKIAexampleAccessKeyId:RTKTl4y68kDcgvayGKxrgybZFXw=");
}

void test_dates()
{
	check_eq("epoch", obs::rfc1123_date(0), "Thu, 01 Jan 1970 00:00:00 GMT");
	check_eq("test date", obs::rfc1123_date(1762128000), test_date);
}

void test_parse_list_objects()
{
	std::string const xml =
		"<ListBucketResult><Name>mybucket</Name><Prefix>dir/</Prefix>"
		"<IsTruncated>true</IsTruncated><NextMarker>dir/sub/</NextMarker>"
		"<Contents><Key>dir/a.txt</Key><Size>11</Size>"
		"<LastModified>2025-11-03T00:00:00.000Z</LastModified>"
		"<ETag>\"ABC\"</ETag><StorageClass>STANDARD</StorageClass></Contents>"
		"<CommonPrefixes><Prefix>dir/sub/</Prefix></CommonPrefixes>"
		"</ListBucketResult>";

	obs::list_result res;
	check_true("list parses", obs::parse_list_objects(xml, res));
	check_eq("prefix", res.prefix, "dir/");
	check_true("truncated", res.truncated);
	check_eq("next marker", res.next_marker, "dir/sub/");
	check_true("two entries", res.entries.size() == 2);
	if (res.entries.size() == 2) {
		check_eq("entry 0 key", res.entries[0].key, "dir/a.txt");
		check_eq_int("entry 0 size", res.entries[0].size, 11);
		check_eq("entry 0 etag", res.entries[0].etag, "\"ABC\"");
		check_true("entry 1 is prefix", res.entries[1].is_prefix);
	}

	obs::list_result bad;
	check_true("garbage fails", !obs::parse_list_objects("nope", bad));
}

void test_parse_buckets_errors_multipart()
{
	std::vector<obs::bucket_entry> buckets;
	check_true("buckets parse", obs::parse_list_buckets(
		"<ListAllMyBucketsResult><Buckets>"
		"<Bucket><Name>b1</Name></Bucket><Bucket><Name>b2</Name></Bucket>"
		"</Buckets></ListAllMyBucketsResult>", buckets));
	check_true("two buckets", buckets.size() == 2);

	std::string code, message;
	check_true("error parses", obs::parse_error(
		"<Error><Code>NoSuchKey</Code><Message>The specified key does not exist.</Message></Error>",
		code, message));
	check_eq("error code", code, "NoSuchKey");

	std::string upload_id;
	check_true("initiate parses", obs::parse_initiate_multipart(
		"<InitiateMultipartUploadResult><Bucket>b</Bucket><Key>k</Key>"
		"<UploadId>OBS-UPLOAD-1</UploadId></InitiateMultipartUploadResult>", upload_id));
	check_eq("upload id", upload_id, "OBS-UPLOAD-1");

	std::vector<obs::part_entry> parts = {{1, "\"e1\""}};
	check_eq("complete body", obs::build_complete_multipart(parts),
		"<CompleteMultipartUpload>"
		"<Part><PartNumber>1</PartNumber><ETag>\"e1\"</ETag></Part>"
		"</CompleteMultipartUpload>");
}

void test_names()
{
	check_eq("encode keeps slash", obs::encode_key("dir/a b.txt"), "dir/a%20b.txt");
	check_eq("encode utf8", obs::encode_key("\xe7\x9b\xae\xe5\xbd\x95.txt"), "%E7%9B%AE%E5%BD%95.txt");
	check_eq("join", obs::join_name("docs/", "a.txt"), "docs/a.txt");
}

}


// Golden StringToSign values published in the Huawei Cloud OBS API reference
// ("Using an Authorization Header", tables 4 to 8).
void test_documented_string_to_sign()
{
	// Table 4: GetObject with a Date header only
	check_eq("doc table 4", obs::string_to_sign("GET", {{"Date", "Sat, 12 Oct 2015 08:12:38 GMT"}},
		"bucket", "object.txt", {}),
		"GET\n\n\nSat, 12 Oct 2015 08:12:38 GMT\n/bucket/object.txt");

	// Table 5: temporary AK/SK with a security token and x-obs-date
	check_eq("doc table 5", obs::string_to_sign("PUT",
		{{"x-obs-date", "Tue, 15 Oct 2015 07:20:09 GMT"},
		 {"x-obs-security-token", "YwkaRTbdY8g7q...."},
		 {"content-type", "text/plain"}},
		"bucket", "object.txt", {}),
		"PUT\n\ntext/plain\n\nx-obs-date:Tue, 15 Oct 2015 07:20:09 GMT\n"
		"x-obs-security-token:YwkaRTbdY8g7q....\n/bucket/object.txt");

	// Table 6: additional x-obs-* header
	check_eq("doc table 6", obs::string_to_sign("PUT",
		{{"Date", "Mon, 14 Oct 2015 12:08:34 GMT"},
		 {"x-obs-acl", "public-read"},
		 {"content-type", "text/plain"}},
		"bucket", "object.txt", {}),
		"PUT\n\ntext/plain\nMon, 14 Oct 2015 12:08:34 GMT\nx-obs-acl:public-read\n/bucket/object.txt");

	// Table 7: sub-resource
	check_eq("doc table 7", obs::string_to_sign("GET", {{"Date", "Sat, 12 Oct 2015 08:12:38 GMT"}},
		"bucket", "object.txt", {{"acl", ""}}),
		"GET\n\n\nSat, 12 Oct 2015 08:12:38 GMT\n/bucket/object.txt?acl");

	// Table 8: Content-MD5
	check_eq("doc table 8", obs::string_to_sign("PUT",
		{{"Content-MD5", "I5pU0r4+sgO9Emgl1KMQUg=="},
		 {"x-obs-date", "Tue, 15 Oct 2015 07:20:09 GMT"}},
		"bucket", "object.txt", {}),
		"PUT\nI5pU0r4+sgO9Emgl1KMQUg==\n\n\nx-obs-date:Tue, 15 Oct 2015 07:20:09 GMT\n/bucket/object.txt");
}
int main()
{
	test_service_and_bucket_signatures();
	test_object_signatures();
	test_multipart_signatures();
	test_header_canonicalization();
	test_dates();
	test_parse_list_objects();
	test_parse_buckets_errors_multipart();
	test_names();
	test_documented_string_to_sign();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures) {
		printf("RESULT: FAILED\n");
		return 1;
	}
	printf("RESULT: OK\n");
	return 0;
}