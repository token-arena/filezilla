#include "../src/engine/dropbox/dropboxjson.h"

#include <libfilezilla/json.hpp>

#include <cstdio>
#include <string>

// Standalone self-test for the Dropbox API v2 logic layer.
// Wired into `make check` as dropboxselftest.

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
  "entries": [
    {"tag": "file", ".tag": "file", "name": "report.pdf", "path_display": "/report.pdf",
     "id": "id:abc", "size": 4096},
    {"tag": "folder", ".tag": "folder", "name": "photos", "path_display": "/photos", "id": "id:def"}
  ],
  "cursor": "AAExampleCursor",
  "has_more": true
})";

void test_list()
{
	dropbox::list_result res;
	std::string error;
	check_true("list parses", dropbox::parse_list(list_json, res, error));
	check_true("two entries", res.entries.size() == 2);
	if (res.entries.size() == 2) {
		check_eq("entry 0 name", res.entries[0].name, "report.pdf");
		check_eq("entry 0 path", res.entries[0].path_display, "/report.pdf");
		check_eq("entry 0 tag", res.entries[0].tag, "file");
		check_eq_int("entry 0 size", res.entries[0].size, 4096);
		check_true("entry 0 not dir", !res.entries[0].is_dir);
		check_true("entry 1 is dir", res.entries[1].is_dir);
	}
	check_eq("cursor", res.cursor, "AAExampleCursor");
	check_true("has more", res.has_more);

	dropbox::list_result empty;
	std::string err2;
	check_true("empty parses", dropbox::parse_list(R"({"entries":[],"cursor":"c","has_more":false})", empty, err2));
	check_true("no entries", empty.entries.empty());
	check_true("not more", !empty.has_more);
}

void test_errors()
{
	std::string message;
	check_true("error summary", dropbox::parse_error(
		R"({"error_summary":"path/not_found/..","error":{".tag":"path"}})", message));
	check_eq("summary text", message, "path/not_found/..");

	std::string m2;
	check_true("tag only", dropbox::parse_error(R"({"error":{".tag":"path"}})", m2));
	check_eq("tag text", m2, "path");
}

void test_escape_non_ascii()
{
	check_eq("ascii untouched", dropbox::escape_non_ascii(R"({"a":"b"})"), R"({"a":"b"})");

	// U+00E4 (ä) = c3 a4
	check_eq("two byte", dropbox::escape_non_ascii("\xc3\xa4"), "\\u00e4");
	// U+76EE (目) = e7 9b ae
	check_eq("three byte", dropbox::escape_non_ascii("\xe7\x9b\xae"), "\\u76ee");
	// U+1F600 (emoji) = f0 9f 98 80 -> surrogate pair
	check_eq("four byte", dropbox::escape_non_ascii("\xf0\x9f\x98\x80"), "\\ud83d\\ude00");
}

void test_paths()
{
	check_eq("join normal", dropbox::join_path("/docs", "a.txt"), "/docs/a.txt");
	check_eq("join root empty", dropbox::join_path("", "a.txt"), "/a.txt");
	check_eq("join root slash", dropbox::join_path("/", "a.txt"), "/a.txt");
	check_eq("join trailing slash", dropbox::join_path("/docs/", "a.txt"), "/docs/a.txt");
}

void test_urls_and_bodies()
{
	check_eq("rpc url", dropbox::rpc_url("files/list_folder"),
		"https://api.dropboxapi.com/2/files/list_folder");
	check_eq("content url", dropbox::content_url("files/upload"),
		"https://content.dropboxapi.com/2/files/upload");

	auto const list = fz::json::parse(dropbox::list_body("/docs"));
	check_eq("list path", list["path"].string_value(), "/docs");
	check_true("list not recursive", !list["recursive"].bool_value());
	check_eq_int("list limit", list["limit"].number_value<int64_t>(0), 1000);

	auto const cont = fz::json::parse(dropbox::continue_body("CURSOR"));
	check_eq("continue cursor", cont["cursor"].string_value(), "CURSOR");

	auto const del = fz::json::parse(dropbox::delete_body("/a.txt"));
	check_eq("delete path", del["path"].string_value(), "/a.txt");

	auto const mk = fz::json::parse(dropbox::create_folder_body("/new"));
	check_eq("mkdir path", mk["path"].string_value(), "/new");
	check_true("mkdir no autorename", !mk["autorename"].bool_value());

	auto const mv = fz::json::parse(dropbox::move_body("/a", "/b"));
	check_eq("move from", mv["from_path"].string_value(), "/a");
	check_eq("move to", mv["to_path"].string_value(), "/b");

	auto const dl = fz::json::parse(dropbox::download_arg("/a.txt"));
	check_eq("download arg path", dl["path"].string_value(), "/a.txt");

	auto const up = fz::json::parse(dropbox::upload_arg("/a.txt", true));
	check_eq("upload mode overwrite", up["mode"].string_value(), "overwrite");
	auto const up2 = fz::json::parse(dropbox::upload_arg("/a.txt", false));
	check_eq("upload mode add", up2["mode"].string_value(), "add");

	// upload arg must be pure ASCII even with non-ASCII names
	auto const up3 = dropbox::upload_arg("/\xe7\x9b\xae.txt", true);
	bool ascii_only = true;
	for (unsigned char const c : up3) {
		if (c >= 0x80) {
			ascii_only = false;
		}
	}
	check_true("upload arg is ascii", ascii_only);
	check_true("upload arg escaped filename", up3.find("\\u76ee") != std::string::npos);
}

}

int main()
{
	test_list();
	test_errors();
	test_escape_non_ascii();
	test_paths();
	test_urls_and_bodies();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures) {
		printf("RESULT: FAILED\n");
		return 1;
	}
	printf("RESULT: OK\n");
	return 0;
}
