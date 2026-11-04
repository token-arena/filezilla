#include "../src/engine/cloudendpoint.h"

#include <cstdio>
#include <string>

// Standalone self-test for the shared cloud endpoint resolution, i.e. the
// HTTP/HTTPS selection used by every cloud protocol.
// Wired into `make check` as endpointselftest.

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

void check_port(char const* what, unsigned short got, unsigned short expected)
{
	++g_checks;
	if (got != expected) {
		++g_failures;
		printf("FAIL %s\n  got: %u expected: %u\n", what, got, expected);
	}
}

void test_resolve()
{
	// Plain host with the protocol default port 443 -> HTTPS
	auto e = cloud::resolve("oss-cn-hangzhou.aliyuncs.com", "", 443);
	check_eq("default scheme", e.scheme, "https");
	check_eq("default host", e.host, "oss-cn-hangzhou.aliyuncs.com");
	check_port("default port", e.port, 443);
	check_eq("default authority", e.authority(), "oss-cn-hangzhou.aliyuncs.com");
	check_eq("default url", e.url(), "https://oss-cn-hangzhou.aliyuncs.com");
	check_port("default uri port", e.uri_port(), 0);
	check_true("default tls", e.tls());

	// MinIO default port 9000 -> plain HTTP
	e = cloud::resolve("minio.local", "", 9000);
	check_eq("minio scheme", e.scheme, "http");
	check_port("minio port", e.port, 9000);
	check_eq("minio authority includes port", e.authority(), "minio.local:9000");
	check_eq("minio url", e.url(), "http://minio.local:9000");
	check_port("minio uri port", e.uri_port(), 9000);

	// Explicit port in the host string
	e = cloud::resolve("192.168.1.10:8080", "", 443);
	check_eq("explicit port scheme", e.scheme, "http");
	check_eq("explicit port host", e.host, "192.168.1.10");
	check_port("explicit port", e.port, 8080);

	// A secure port means HTTPS
	e = cloud::resolve("example.com:8443", "", 80);
	check_eq("8443 scheme", e.scheme, "https");
	check_port("8443 port", e.port, 8443);

	// Full URL wins over the default
	e = cloud::resolve("http://gateway.example.com:8080/ignored/path", "", 443);
	check_eq("url scheme", e.scheme, "http");
	check_eq("url host", e.host, "gateway.example.com");
	check_port("url port", e.port, 8080);

	// The tls parameter overrides everything else
	e = cloud::resolve("bucket-1250000000.cos.ap-beijing.myqcloud.com", "0", 443);
	check_eq("tls=0 scheme", e.scheme, "http");
	check_port("tls=0 port", e.port, 443);
	check_eq("tls=0 authority keeps port", e.authority(), "bucket-1250000000.cos.ap-beijing.myqcloud.com:443");

	e = cloud::resolve("minio.local:9000", "1", 9000);
	check_eq("tls=1 scheme", e.scheme, "https");
	check_eq("tls=1 authority", e.authority(), "minio.local:9000");

	// HTTP default port 80 must not be repeated in the authority
	e = cloud::resolve("legacy.local", "0", 80);
	check_eq("http 80 authority", e.authority(), "legacy.local");
	check_port("http 80 uri port", e.uri_port(), 0);

	// IPv6 literal with a port
	e = cloud::resolve("[::1]:9000", "", 443);
	check_eq("ipv6 host", e.host, "[::1]");
	check_port("ipv6 port", e.port, 9000);

	// Garbage port numbers are ignored, the host stays intact
	e = cloud::resolve("weird:host", "", 443);
	check_eq("garbage port host", e.host, "weird:host");
	check_port("garbage port default", e.port, 443);
}

void test_flags()
{
	bool flag = false;
	check_true("parse 1", cloud::parse_flag("1", flag) && flag);
	check_true("parse true", cloud::parse_flag("TRUE", flag) && flag);
	check_true("parse yes", cloud::parse_flag("yes", flag) && flag);
	check_true("parse 0", cloud::parse_flag("0", flag) && !flag);
	check_true("parse no", cloud::parse_flag("no", flag) && !flag);
	check_true("parse https", cloud::parse_flag("https", flag) && flag);
	check_true("parse http", cloud::parse_flag("http", flag) && !flag);
	check_true("parse empty is unknown", !cloud::parse_flag("", flag));
	check_true("parse garbage is unknown", !cloud::parse_flag("maybe", flag));
}

void test_with_host()
{
	// Fixed API base, redirected to a private gateway
	auto base = cloud::resolve("storage.googleapis.com", "", 443);
	auto e = cloud::with_host(base, "gcs-gateway.internal:8080");
	check_eq("with_host scheme", e.scheme, "http");
	check_eq("with_host host", e.host, "gcs-gateway.internal");
	check_port("with_host port", e.port, 8080);

	// Empty override keeps the base untouched
	e = cloud::with_host(base, "");
	check_eq("empty override keeps base", e.url(), "https://storage.googleapis.com");

	// Override carrying its own scheme
	e = cloud::with_host(base, "http://proxy.internal");
	check_eq("override scheme", e.scheme, "http");
	check_eq("override host", e.host, "proxy.internal");
	check_port("override keeps base port", e.port, 443);
}

}

void test_rebase()
{
	auto ep = cloud::resolve("gcs-gateway.internal:8080", "", 443);
	check_eq("rebase keeps path and query",
		cloud::rebase("https://storage.googleapis.com/storage/v1/b?project=x", ep),
		"http://gcs-gateway.internal:8080/storage/v1/b?project=x");
	check_eq("rebase without path", cloud::rebase("https://api.dropboxapi.com", ep),
		"http://gcs-gateway.internal:8080");
}

int main()
{
	test_resolve();
	test_flags();
	test_with_host();
	test_rebase();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures) {
		printf("RESULT: FAILED\n");
		return 1;
	}
	printf("RESULT: OK\n");
	return 0;
}
