#include "../src/engine/gdrive/drivejson.h"

#include <libfilezilla/json.hpp>

#include <cstdio>
#include <string>

// Standalone self-test for the Google Drive logic layer (API v3).
// Wired into `make check` as driveselftest.

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
  "files": [
    {"id": "id1", "name": "report.pdf", "mimeType": "application/pdf", "size": "4096",
     "modifiedTime": "2025-11-03T00:00:00.000Z"},
    {"id": "id2", "name": "photos", "mimeType": "application/vnd.google-apps.folder"},
    {"id": "", "name": "broken"}
  ],
  "nextPageToken": "next-1"
})";

void test_list_files()
{
	std::vector<gdrive::file_entry> files;
	std::string token, error;
	check_true("list parses", gdrive::parse_list_files(list_json, files, token, error));
	check_true("two valid entries", files.size() == 2);
	if (files.size() == 2) {
		check_eq("file 0 id", files[0].id, "id1");
		check_eq("file 0 name", files[0].name, "report.pdf");
		check_eq_int("file 0 size", files[0].size, 4096);
		check_true("file 0 is not a dir", !files[0].is_dir);
		check_true("file 1 is a dir", files[1].is_dir);
	}
	check_eq("next page token", token, "next-1");

	std::vector<gdrive::file_entry> empty;
	std::string t2, e2;
	check_true("empty listing parses", gdrive::parse_list_files(R"({"files":[]})", empty, t2, e2));
	check_true("no entries", empty.empty());
}

void test_parse_error()
{
	std::string message;
	check_true("error parses", gdrive::parse_error(
		R"({"error":{"code":404,"message":"File not found: id1."}})", message));
	check_eq("error message", message, "File not found: id1.");

	std::string m2;
	check_true("plain doc is not an error", !gdrive::parse_error(R"({"files":[]})", m2));
}

void test_queries()
{
	check_eq("folder query",
		gdrive::folder_query("rootId", "photos"),
		"'rootId' in parents and name = 'photos' and mimeType = 'application/vnd.google-apps.folder' and trashed = false");

	// Quotes and backslashes must be escaped
	check_eq("escape quotes", gdrive::escape_query_value("it's"), "it\\'s");
	check_eq("escape backslash", gdrive::escape_query_value("a\\b"), "a\\\\b");

	check_eq("children query", gdrive::children_query("root"), "'root' in parents and trashed = false");
}

void test_urls()
{
	check_eq("file url", gdrive::file_url("id1"), "https://www.googleapis.com/drive/v3/files/id1");
	check_eq("media url", gdrive::media_url("id1"), "https://www.googleapis.com/drive/v3/files/id1?alt=media");
	check_eq("upload url", gdrive::upload_url("id1"),
		"https://www.googleapis.com/upload/drive/v3/files/id1?uploadType=media");
	check_eq("create url", gdrive::create_url(), "https://www.googleapis.com/drive/v3/files");
	check_eq("delete url", gdrive::delete_url("a/b"), "https://www.googleapis.com/drive/v3/files/a%2Fb");

	auto const list = gdrive::list_url("root", "tok");
	check_true("list url starts correct",
		list.rfind("https://www.googleapis.com/drive/v3/files?q=", 0) == 0);
	check_true("list url contains page token", list.find("&pageToken=tok") != std::string::npos);
	check_true("list url encodes query", list.find("%20in%20parents") != std::string::npos);

	auto const search = gdrive::search_url(gdrive::folder_query("root", "a b"));
	check_true("search url encodes space", search.find("a%20b") != std::string::npos);
	check_true("search url encodes quote", search.find("%27") != std::string::npos);
}

void test_bodies()
{
	auto const folder = fz::json::parse(gdrive::create_folder_body("newdir", "parent1"));
	check_eq("folder body name", folder["name"].string_value(), "newdir");
	check_eq("folder body mime", folder["mimeType"].string_value(), gdrive::folder_mime_type);
	check_eq("folder body parent", folder["parents"][0].string_value(), "parent1");

	auto const file = fz::json::parse(gdrive::create_file_body("a.txt", "parent2"));
	check_eq("file body name", file["name"].string_value(), "a.txt");
	check_eq("file body parent", file["parents"][0].string_value(), "parent2");

	auto const renamed = fz::json::parse(gdrive::rename_body("b.txt"));
	check_eq("rename body", renamed["name"].string_value(), "b.txt");

	auto const moved = fz::json::parse(gdrive::move_body("newparent", "oldparent"));
	check_eq("move add", moved["addParents"].string_value(), "newparent");
	check_eq("move remove", moved["removeParents"].string_value(), "oldparent");
}

}

int main()
{
	test_list_files();
	test_parse_error();
	test_queries();
	test_urls();
	test_bodies();

	printf("\n%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures) {
		printf("RESULT: FAILED\n");
		return 1;
	}
	printf("RESULT: OK\n");
	return 0;
}
