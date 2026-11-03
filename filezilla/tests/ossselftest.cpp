#include "../src/engine/oss/ossrequest.h"

#include <cstdio>
#include <string>

// Standalone self-test for the Alibaba Cloud OSS logic layer (signature V1,
// XML parsing). Wired into `make check` as ossselftest.
//
// Signature vectors were produced by an independent Python implementation of the
// documented OSS signature V1 algorithm and then cross-checked here.

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

oss::credentials test_credentials()
{
	oss::credentials c;
	c.access_key = "LTAIexampleAccessKeyId";
	c.secret_key = "exampleSecretAccessKeyForTesting1234567890";
	return c;
}

std::string const test_date = "Mon, 03 Nov 2025 00:00:00 GMT";

std::map<std::string, std::string> base_headers()
{
	return {{"Date", test_date}};
}

void test_list_containers_signature()
{
	// Service level request: canonical resource is just "/"
	check_eq("service string to sign",
		oss::string_to_sign("GET", base_headers(), "", "", {}),
		"GET\n\n\n" + test_date + "\n/");
	check_eq("service signature",
		oss::sign(test_credentials(), "GET", base_headers(), "", "", {}),
		"OSS LTAIexampleAccessKeyId:MnnKavxHJMVZa68BfCbLfQe6p/w=");
}

void test_list_objects_signature()
{
	check_eq("list objects signature",
		oss::sign(test_credentials(), "GET", base_headers(), "mybucket", "", {}),
		"OSS LTAIexampleAccessKeyId:ZJgDLrTCWN+KoqNN4zOafT6xWuk=");
}

void test_put_get_delete_signatures()
{
	auto headers = base_headers();
	headers["Content-Type"] = "text/plain";
	headers["Content-Length"] = "13";

	check_eq("put string to sign",
		oss::string_to_sign("PUT", headers, "mybucket", "dir/file.txt", {}),
		"PUT\n\ntext/plain\n" + test_date + "\n/mybucket/dir/file.txt");
	check_eq("put signature",
		oss::sign(test_credentials(), "PUT", headers, "mybucket", "dir/file.txt", {}),
		"OSS LTAIexampleAccessKeyId:CPK42vYbO8e7o8upwuDYaFECqeQ=");

	check_eq("get signature",
		oss::sign(test_credentials(), "GET", base_headers(), "mybucket", "dir/file.txt", {}),
		"OSS LTAIexampleAccessKeyId:Hss29aumHt5xA875R/S8c+5J6mI=");

	check_eq("delete signature",
		oss::sign(test_credentials(), "DELETE", base_headers(), "mybucket", "dir/file.txt", {}),
		"OSS LTAIexampleAccessKeyId:H/yHEy4Xk1kPh+2xrAFxZvX+PB4=");
}

void test_multipart_signatures()
{
	check_eq("initiate multipart signature",
		oss::sign(test_credentials(), "POST", base_headers(), "mybucket", "big.bin", {{"uploads", ""}}),
		"OSS LTAIexampleAccessKeyId:4Ms2NRJEy90YS9WfWDv1iuQKrMI=");

	std::vector<std::pair<std::string, std::string>> part_subs = {
		{"partNumber", "1"}, {"uploadId", "0004B9894A22E5B1888A1E29F8236E2D"}};
	check_eq("upload part string to sign",
		oss::string_to_sign("PUT", base_headers(), "mybucket", "big.bin", part_subs),
		"PUT\n\n\n" + test_date + "\n/mybucket/big.bin\npartnumber=1\nuploadid=0004B9894A22E5B1888A1E29F8236E2D");
	check_eq("upload part signature",
		oss::sign(test_credentials(), "PUT", base_headers(), "mybucket", "big.bin", part_subs),
		"OSS LTAIexampleAccessKeyId:AC2ZmAat5k4XQx0exacZAiAqX5U=");

	auto headers = base_headers();
	headers["Content-Type"] = "application/xml";
	check_eq("complete multipart signature",
		oss::sign(test_credentials(), "POST", headers, "mybucket", "big.bin",
			{{"uploadId", "0004B9894A22E5B1888A1E29F8236E2D"}}),
		"OSS LTAIexampleAccessKeyId:LXR5fB/3xUoREUeGg6nK0TtUSFk=");
}

void test_canonicalized_headers()
{
	auto headers = base_headers();
	headers["x-oss-copy-source"] = "/mybucket/src.txt";
	check_eq("copy object signature",
		oss::sign(test_credentials(), "PUT", headers, "mybucket", "dst.txt", {}),
		"OSS LTAIexampleAccessKeyId:ZPQkyDsr/HMBycrNWgQ1a23XOuk=");

	// Canonicalized x-oss-* headers must be lowercased and sorted
	std::map<std::string, std::string> h = {
		{"x-oss-meta-b", "2"},
		{"X-OSS-META-A", "1"},
	};
	check_eq("canonicalized oss headers", oss::canonicalized_oss_headers(h),
		"x-oss-meta-a:1\nx-oss-meta-b:2\n");

	// STS token participates in the signature via canonicalized headers
	auto sts_headers = base_headers();
	sts_headers["x-oss-security-token"] = "STS-TOKEN";
	check_eq("sts token signature",
		oss::sign(test_credentials(), "GET", sts_headers, "mybucket", "a.txt", {}),
		"OSS LTAIexampleAccessKeyId:+NhP8XJHyLcWWjSwBdvTi26zIBI=");
}

void test_dates()
{
	check_eq("epoch", oss::rfc1123_date(0), "Thu, 01 Jan 1970 00:00:00 GMT");
	check_eq("test date", oss::rfc1123_date(1762128000), test_date);
}

void test_parse_list_objects()
{
	std::string const xml =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
		"<ListBucketResult><Name>mybucket</Name><Prefix>dir/</Prefix><Delimiter>/</Delimiter>"
		"<MaxKeys>1000</MaxKeys><IsTruncated>true</IsTruncated><NextMarker>dir/sub/</NextMarker>"
		"<Contents><Key>dir/a.txt</Key><Size>11</Size>"
		"<LastModified>2025-11-03T00:00:00.000Z</LastModified>"
		"<ETag>\"ABC\"</ETag><StorageClass>Standard</StorageClass></Contents>"
		"<CommonPrefixes><Prefix>dir/sub/</Prefix></CommonPrefixes>"
		"</ListBucketResult>";

	oss::list_result res;
	check_true("list objects parses", oss::parse_list_objects(xml, res));
	check_eq("bucket", res.bucket, "mybucket");
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

	oss::list_result bad;
	check_true("garbage fails", !oss::parse_list_objects("nope", bad));
}

void test_parse_buckets_and_error()
{
	std::vector<oss::bucket_entry> buckets;
	check_true("buckets parse", oss::parse_list_buckets(
		"<ListAllMyBucketsResult><Buckets>"
		"<Bucket><Name>b1</Name><Location>oss-cn-hangzhou</Location></Bucket>"
		"<Bucket><Name>b2</Name></Bucket>"
		"</Buckets></ListAllMyBucketsResult>", buckets));
	check_true("two buckets", buckets.size() == 2);
	if (!buckets.empty()) {
		check_eq("bucket 0 name", buckets[0].name, "b1");
		check_eq("bucket 0 location", buckets[0].location, "oss-cn-hangzhou");
	}

	std::string code, message;
	check_true("error parses", oss::parse_error(
		"<Error><Code>NoSuchKey</Code><Message>The specified key does not exist.</Message></Error>",
		code, message));
	check_eq("error code", code, "NoSuchKey");
	check_true("error message set", !message.empty());
}

void test_multipart_xml()
{
	std::string upload_id;
	check_true("initiate parses", oss::parse_initiate_multipart(
		"<InitiateMultipartUploadResult><Bucket>b</Bucket><Key>k</Key>"
		"<UploadId>UPLOAD123</UploadId></InitiateMultipartUploadResult>", upload_id));
	check_eq("upload id", upload_id, "UPLOAD123");

	std::vector<oss::part_entry> parts = {{1, "\"e1\""}, {2, "\"e2\""}};
	check_eq("complete body", oss::build_complete_multipart(parts),
		"<CompleteMultipartUpload>"
		"<Part><PartNumber>1</PartNumber><ETag>\"e1\"</ETag></Part>"
		"<Part><PartNumber>2</PartNumber><ETag>\"e2\"</ETag></Part>"
		"</CompleteMultipartUpload>");
}

void test_names()
{
	check_eq("encode keeps slash", oss::encode_key("dir/a b.txt"), "dir/a%20b.txt");
	check_eq("encode utf8", oss::encode_key("\xe7\x9b\xae\xe5\xbd\x95.txt"), "%E7%9B%AE%E5%BD%95.txt");
	check_eq("join", oss::join_name("docs/", "a.txt"), "docs/a.txt");
	check_eq("join root", oss::join_name("", "a.txt"), "a.txt");
}

}

int main()
{
	test_list_containers_signature();
	test_list_objects_signature();
	test_put_get_delete_signatures();
	test_multipart_signatures();
	test_canonicalized_headers();
	test_dates();
	test_parse_list_objects();
	test_parse_buckets_and_error();
	test_multipart_xml();
	test_names();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures) {
		printf("RESULT: FAILED\n");
		return 1;
	}
	printf("RESULT: OK\n");
	return 0;
}
