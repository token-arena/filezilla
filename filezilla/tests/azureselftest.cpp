#include "../src/engine/azure/azureblob.h"

#include <cstdio>
#include <string>

// Standalone self-test for the Azure Blob Storage logic layer (SharedKey signing,
// XML parsing, date formatting). Wired into `make check` as azureselftest.

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

azure::credentials test_credentials()
{
	azure::credentials c;
	c.account = "myaccount";
	// base64 of "0123456789abcdef0123456789abcdef"
	c.key_base64 = "MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWY=";
	return c;
}

std::map<std::string, std::string> base_headers()
{
	return {
		{"x-ms-date", "Mon, 03 Nov 2025 00:00:00 GMT"},
		{"x-ms-version", "2021-08-06"},
	};
}

// Reference values computed independently with a Python implementation of the
// SharedKey specification.
void test_list_containers_signature()
{
	auto headers = base_headers();
	std::vector<std::pair<std::string, std::string>> query = {{"comp", "list"}};

	check_eq("list containers string to sign",
		azure::string_to_sign("GET", headers, "myaccount", "/", query),
		"GET\n\n\n\n\n\n\n\n\n\n\n\n"
		"x-ms-date:Mon, 03 Nov 2025 00:00:00 GMT\n"
		"x-ms-version:2021-08-06\n"
		"/myaccount/\n"
		"comp:list");

	check_eq("list containers authorization",
		azure::sign(test_credentials(), "GET", headers, "/", query),
		"SharedKey myaccount:KUXx9s2kHi0qyX6bR5IMgG2CGGHEZhqRb6bbGkl+yas=");
}

void test_list_blobs_signature()
{
	auto headers = base_headers();
	std::vector<std::pair<std::string, std::string>> query = {
		{"restype", "container"}, {"comp", "list"}, {"prefix", "photos/"}, {"delimiter", "/"}};

	check_eq("list blobs authorization",
		azure::sign(test_credentials(), "GET", headers, "/mycontainer", query),
		"SharedKey myaccount:fMwNWoj9UWrVnY2SpP7fQaNMVxia14C+8YQo0A4xuqk=");

	// Query parameters must end up sorted by lowercase name
	check_eq("list blobs canonicalized resource",
		azure::canonicalized_resource("myaccount", "/mycontainer", query),
		"/myaccount/mycontainer\ncomp:list\ndelimiter:/\nprefix:photos/\nrestype:container");
}

void test_put_blob_signature()
{
	auto headers = base_headers();
	headers["x-ms-blob-type"] = "BlockBlob";
	headers["Content-Length"] = "13";
	headers["Content-Type"] = "text/plain";

	check_eq("put blob authorization",
		azure::sign(test_credentials(), "PUT", headers, "/mycontainer/dir/file.txt", {}),
		"SharedKey myaccount:eqx0gkBvs+31ZuTdTUgUcAO5q6T3dubInP4WgWx9IPQ=");
}

void test_get_range_signature()
{
	auto headers = base_headers();
	headers["Range"] = "bytes=0-99";

	check_eq("get blob range authorization",
		azure::sign(test_credentials(), "GET", headers, "/mycontainer/dir/file.txt", {}),
		"SharedKey myaccount:Sw5uPUW0cYuwzrbzGR7xhLbO2PYjfAq+0NaZGuMICBk=");
}

void test_delete_signature()
{
	check_eq("delete blob authorization",
		azure::sign(test_credentials(), "DELETE", base_headers(), "/mycontainer/dir/file.txt", {}),
		"SharedKey myaccount:CB8cV0WS8YAMalPMkVpsy0JBW29J4tJw5zTcOVRnfoo=");
}

void test_zero_content_length()
{
	// An explicit Content-Length of 0 must be treated like an empty value
	auto headers = base_headers();
	headers["Content-Length"] = "0";
	headers["x-ms-blob-type"] = "BlockBlob";

	auto const with_zero = azure::string_to_sign("PUT", headers, "myaccount", "/c/b", {});
	headers.erase("Content-Length");
	auto const without = azure::string_to_sign("PUT", headers, "myaccount", "/c/b", {});
	check_eq("zero content length equals absent", with_zero, without);
}

void test_dates()
{
	check_eq("epoch", azure::rfc1123_date(0), "Thu, 01 Jan 1970 00:00:00 GMT");
	check_eq("2025-11-03", azure::rfc1123_date(1762128000), "Mon, 03 Nov 2025 00:00:00 GMT");
}

void test_parse_list_blobs()
{
	std::string const xml =
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		"<EnumerationResults ServiceEndpoint=\"https://myaccount.blob.core.windows.net/\">"
		"<Blobs>"
		"<Blob><Name>photos/a.jpg</Name><Properties>"
		"<Last-Modified>Mon, 03 Nov 2025 00:00:00 GMT</Last-Modified>"
		"<Content-Length>1024</Content-Length><Content-Type>image/jpeg</Content-Type>"
		"</Properties></Blob>"
		"<BlobPrefix><Name>photos/sub/</Name></BlobPrefix>"
		"</Blobs>"
		"<NextMarker>marker123</NextMarker>"
		"</EnumerationResults>";

	azure::list_blobs_result res;
	check_true("list blobs parses", azure::parse_list_blobs(xml, res));
	check_true("two entries", res.blobs.size() == 2);
	if (res.blobs.size() == 2) {
		check_eq("blob name", res.blobs[0].name, "photos/a.jpg");
		check_eq_int("blob size", res.blobs[0].size, 1024);
		check_eq("blob content type", res.blobs[0].content_type, "image/jpeg");
		check_true("blob 0 not prefix", !res.blobs[0].is_prefix);
		check_true("blob 1 is prefix", res.blobs[1].is_prefix);
		check_eq("prefix name", res.blobs[1].name, "photos/sub/");
	}
	check_eq("next marker", res.next_marker, "marker123");

	azure::list_blobs_result bad;
	check_true("garbage fails", !azure::parse_list_blobs("not xml", bad));
}

void test_parse_list_containers()
{
	std::string const xml =
		"<EnumerationResults><Containers>"
		"<Container><Name>first</Name><Properties>"
		"<Last-Modified>Mon, 03 Nov 2025 00:00:00 GMT</Last-Modified></Properties></Container>"
		"<Container><Name>second</Name></Container>"
		"</Containers><NextMarker /></EnumerationResults>";

	std::vector<azure::container_entry> containers;
	std::string marker;
	check_true("list containers parses", azure::parse_list_containers(xml, containers, marker));
	check_true("two containers", containers.size() == 2);
	if (containers.size() == 2) {
		check_eq("container 0", containers[0].name, "first");
		check_eq("container 1", containers[1].name, "second");
	}
	check_true("empty next marker", marker.empty());
}

void test_parse_error()
{
	std::string code, message;
	check_true("error parses", azure::parse_error(
		"<Error><Code>BlobNotFound</Code><Message>The specified blob does not exist.</Message></Error>",
		code, message));
	check_eq("error code", code, "BlobNotFound");
	check_eq("error message", message, "The specified blob does not exist.");

	std::string c2, m2;
	check_true("garbage is not an error doc", !azure::parse_error("<Other/>", c2, m2));
}

void test_names()
{
	check_eq("encode keeps slash", azure::encode_blob_name("dir/a b.txt"), "dir/a%20b.txt");
	check_eq("encode utf8", azure::encode_blob_name("\xe7\x9b\xae\xe5\xbd\x95.txt"), "%E7%9B%AE%E5%BD%95.txt");
	check_eq("join with prefix", azure::join_name("docs/", "a.txt"), "docs/a.txt");
	check_eq("join without slash", azure::join_name("docs", "a.txt"), "docs/a.txt");
	check_eq("join empty prefix", azure::join_name("", "a.txt"), "a.txt");
	check_eq("join strips leading slash", azure::join_name("docs/", "/a.txt"), "docs/a.txt");
}

}

int main()
{
	test_list_containers_signature();
	test_list_blobs_signature();
	test_put_blob_signature();
	test_get_range_signature();
	test_delete_signature();
	test_zero_content_length();
	test_dates();
	test_parse_list_blobs();
	test_parse_list_containers();
	test_parse_error();
	test_names();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures) {
		printf("RESULT: FAILED\n");
		return 1;
	}
	printf("RESULT: OK\n");
	return 0;
}
