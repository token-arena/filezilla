#include "../src/engine/s3/s3response.h"
#include "../src/engine/s3/s3signer.h"

#include <cstdio>
#include <string>

// Standalone self-test for the S3 protocol logic layers (signing + XML parsing).
// Deliberately free of the engine so it can be built and run in seconds, see
// _work/s3-selftest.sh for the exact compiler invocation used during development.
// It is also wired into `make check` as the s3selftest program.

namespace {

int g_failures = 0;
int g_checks = 0;

void fail(char const* what, std::string const& got, std::string const& expected)
{
	++g_failures;
	printf("FAIL %s\n  got:      %s\n  expected: %s\n", what, got.c_str(), expected.c_str());
}

void check_eq(char const* what, std::string const& got, std::string const& expected)
{
	++g_checks;
	if (got != expected) {
		fail(what, got, expected);
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

s3::credentials test_credentials()
{
	s3::credentials c;
	c.access_key = "AKIDEXAMPLE";
	c.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
	return c;
}

// Reference vectors generated independently with a separate implementation of the
// SigV4 specification in Python (and cross-checked against the example signature
// published in the AWS S3 documentation).
void test_uri_encode()
{
	check_eq("uri_encode ascii", s3::uri_encode("aZ0-._~"), "aZ0-._~");
	check_eq("uri_encode space", s3::uri_encode("a b"), "a%20b");
	check_eq("uri_encode slash encoded", s3::uri_encode("photos/2006"), "photos%2F2006");
	check_eq("uri_encode slash kept", s3::uri_encode("photos/2006", true), "photos/2006");
	check_eq("uri_encode utf8", s3::uri_encode("\xe7\x9b\xae\xe5\xbd\x95"), "%E7%9B%AE%E5%BD%95");
	check_eq("uri_encode reserved", s3::uri_encode("a+b=c&d?e#f"), "a%2Bb%3Dc%26d%3Fe%23f");
}

void test_canonical_query()
{
	std::vector<std::pair<std::string, std::string>> params = {
		{"max-keys", "2"},
		{"list-type", "2"},
		{"prefix", "photos/"},
		{"encoding-type", "url"},
	};
	check_eq("canonical query", s3::canonical_query_string(params),
		"encoding-type=url&list-type=2&max-keys=2&prefix=photos%2F");

	std::vector<std::pair<std::string, std::string>> empty_params;
	check_eq("canonical query empty", s3::canonical_query_string(empty_params), "");
}

void test_hash_helpers()
{
	check_eq("sha256 of empty string", s3::sha256_hex(""),
		"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	check_eq("hmac sanity", s3::hmac_sha256_hex("key", "The quick brown fox jumps over the lazy dog"),
		"f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
}

void test_dates()
{
	check_eq("format epoch", s3::format_amz_date(0), "19700101T000000Z");
	check_eq("format 2013-05-24", s3::format_amz_date(1369353600), "20130524T000000Z");
	check_eq("format leap day", s3::format_amz_date(1709251199), "20240229T235959Z");
}

// AWS documentation example: GET Object with a Range header.
// https://docs.aws.amazon.com/AmazonS3/latest/API/sig-v4-header-based-auth.html
void test_aws_documented_example()
{
	s3::signable_request req;
	req.method = "GET";
	req.host = "examplebucket.s3.amazonaws.com";
	req.canonical_uri = "/test.txt";
	req.payload_hash = s3::sha256_hex("");
	req.headers["range"] = "bytes=0-9";

	auto const res = s3::sign(req, test_credentials(), "us-east-1", "s3", "20130524T000000Z");

	check_eq("aws example signature", res.signature,
		"f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
	check_eq("aws example signed headers", res.signed_headers,
		"host;range;x-amz-content-sha256;x-amz-date");
	check_eq("aws example authorization", res.authorization,
		"AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20130524/us-east-1/s3/aws4_request, "
		"SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
		"Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
}

void test_list_request_signature()
{
	s3::signable_request req;
	req.method = "GET";
	req.host = "examplebucket.s3.amazonaws.com";
	req.canonical_uri = "/";
	req.payload_hash = s3::sha256_hex("");
	req.query = {
		{"list-type", "2"},
		{"prefix", "photos/"},
		{"max-keys", "2"},
		{"encoding-type", "url"},
	};

	auto const res = s3::sign(req, test_credentials(), "us-east-1", "s3", "20130524T000000Z");
	check_eq("list signature", res.signature,
		"685e39dd23703e38efec340d993eb539ef51966e51ee92be857996647190b0b3");
}

void test_put_request_signature()
{
	s3::signable_request req;
	req.method = "PUT";
	req.host = "examplebucket.s3.amazonaws.com";
	req.canonical_uri = "/test%24file.text";
	req.payload_hash = s3::sha256_hex("Welcome to Amazon S3.");
	req.headers["content-type"] = "text/plain";

	auto const res = s3::sign(req, test_credentials(), "us-east-1", "s3", "20130524T000000Z");

	check_eq("put payload hash", res.canonical_request.substr(res.canonical_request.rfind('\n') + 1),
		"44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072");
	check_eq("put signature", res.signature,
		"e1597ff9ce11084ff783c57808e6d23a187daf25adfeb51196d145185c480704");
}

void test_path_style_unicode_signature()
{
	s3::signable_request req;
	req.method = "GET";
	req.host = "localhost:9000";
	req.canonical_uri = "/mybucket/" + s3::uri_encode("\xe7\x9b\xae\xe5\xbd\x95/a b.txt", true);
	req.payload_hash = s3::sha256_hex("");

	check_eq("path-style uri", req.canonical_uri, "/mybucket/%E7%9B%AE%E5%BD%95/a%20b.txt");

	auto const res = s3::sign(req, test_credentials(), "us-east-1", "s3", "20130524T000000Z");
	check_eq("path-style signature", res.signature,
		"2bc8241d66115ac1743059859275e457290c2b1bcfa1998069174c75797b30ae");
}

void test_session_token_signature()
{
	s3::signable_request req;
	req.method = "GET";
	req.host = "examplebucket.s3.amazonaws.com";
	req.canonical_uri = "/";
	req.payload_hash = s3::sha256_hex("");

	auto cred = test_credentials();
	cred.session_token = "SESSIONTOKEN";

	auto const res = s3::sign(req, cred, "eu-central-1", "s3", "20130524T000000Z");

	check_eq("session token signature", res.signature,
		"c0b056fae204575739c59f25fbb965d075daf69abaea23624a1183083c2e94a1");
	check_eq("session token signed headers", res.signed_headers,
		"host;x-amz-content-sha256;x-amz-date;x-amz-security-token");
}

void test_parse_list_objects_v2()
{
	std::string const xml =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">"
		"<Name>examplebucket</Name>"
		"<Prefix></Prefix>"
		"<KeyCount>3</KeyCount>"
		"<MaxKeys>2</MaxKeys>"
		"<IsTruncated>true</IsTruncated>"
		"<NextContinuationToken>token123</NextContinuationToken>"
		"<Contents>"
		"<Key>test.txt</Key>"
		"<LastModified>2013-05-24T00:00:00.000Z</LastModified>"
		"<ETag>&quot;abc&quot;</ETag>"
		"<Size>11</Size>"
		"<StorageClass>STANDARD</StorageClass>"
		"</Contents>"
		"<Contents>"
		"<Key>photos/2006/january/</Key>"
		"<LastModified>2013-05-24T00:00:00.000Z</LastModified>"
		"<ETag>&quot;def&quot;</ETag>"
		"<Size>0</Size>"
		"</Contents>"
		"<CommonPrefixes><Prefix>photos/</Prefix></CommonPrefixes>"
		"</ListBucketResult>";

	s3::list_result result;
	check_true("parse list v2 returns true", s3::parse_list_objects(xml, result));
	check_eq("list bucket", result.bucket, "examplebucket");
	check_true("list truncated", result.truncated);
	check_eq("list next token", result.next_continuation_token, "token123");
	check_true("list entry count", result.entries.size() == 3);
	if (result.entries.size() == 3) {
		check_eq("entry 0 key", result.entries[0].key, "test.txt");
		check_true("entry 0 size", result.entries[0].size == 11);
		check_eq("entry 0 etag", result.entries[0].etag, "\"abc\"");
		check_eq("entry 0 storage class", result.entries[0].storage_class, "STANDARD");
		check_eq("entry 1 key", result.entries[1].key, "photos/2006/january/");
		check_true("entry 2 is prefix", result.entries[2].is_prefix);
		check_eq("entry 2 key", result.entries[2].key, "photos/");
	}

	std::string const empty_list =
		"<ListBucketResult><Name>b</Name><IsTruncated>false</IsTruncated></ListBucketResult>";
	s3::list_result empty;
	check_true("parse empty list", s3::parse_list_objects(empty_list, empty));
	check_true("empty list has no entries", empty.entries.empty());
	check_true("empty list not truncated", !empty.truncated);
}

void test_parse_error()
{
	std::string const xml =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
		"<Error><Code>NoSuchKey</Code><Message>The specified key does not exist.</Message>"
		"<RequestId>4442587FB7D0A2F9</RequestId><Resource>/mybucket/missing.txt</Resource></Error>";

	s3::error_info info;
	check_true("parse error returns true", s3::parse_error(xml, info));
	check_eq("error code", info.code, "NoSuchKey");
	check_eq("error message", info.message, "The specified key does not exist.");
	check_eq("error request id", info.request_id, "4442587FB7D0A2F9");

	s3::error_info bad;
	check_true("parse garbage fails", !s3::parse_error("not xml at all", bad));
	check_true("parse empty fails", !s3::parse_error("", bad));
}

void test_multipart()
{
	std::string const initiate =
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
		"<InitiateMultipartUploadResult><Bucket>mybucket</Bucket><Key>big.bin</Key>"
		"<UploadId>VXBsb2FkSWQ</UploadId></InitiateMultipartUploadResult>";

	std::string upload_id;
	check_true("initiate parses", s3::parse_initiate_multipart(initiate, upload_id));
	check_eq("upload id", upload_id, "VXBsb2FkSWQ");

	std::string missing;
	check_true("initiate without id fails",
		!s3::parse_initiate_multipart("<InitiateMultipartUploadResult><Bucket>b</Bucket></InitiateMultipartUploadResult>", missing));
	check_true("initiate garbage fails", !s3::parse_initiate_multipart("nope", missing));

	std::vector<s3::part_entry> parts = {{1, "\"etag-one\""}, {2, "\"etag-two\""}};
	check_eq("complete body", s3::build_complete_multipart(parts),
		"<CompleteMultipartUpload>"
		"<Part><PartNumber>1</PartNumber><ETag>\"etag-one\"</ETag></Part>"
		"<Part><PartNumber>2</PartNumber><ETag>\"etag-two\"</ETag></Part>"
		"</CompleteMultipartUpload>");

	std::vector<s3::part_entry> no_parts;
	check_eq("complete body empty", s3::build_complete_multipart(no_parts),
		"<CompleteMultipartUpload></CompleteMultipartUpload>");
}

}

int main()
{
	test_uri_encode();
	test_canonical_query();
	test_hash_helpers();
	test_dates();
	test_aws_documented_example();
	test_list_request_signature();
	test_put_request_signature();
	test_path_style_unicode_signature();
	test_session_token_signature();
	test_parse_list_objects_v2();
	test_parse_error();
	test_multipart();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures) {
		printf("RESULT: FAILED\n");
		return 1;
	}
	printf("RESULT: OK\n");
	return 0;
}
