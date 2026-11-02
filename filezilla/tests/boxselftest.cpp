#include "../src/engine/box/boxjson.h"

#include <libfilezilla/json.hpp>

#include <cstdio>
#include <string>

// Standalone self-test for the Box API logic layer.
// Wired into `make check` as boxselftest.

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
  "total_count": 3,
  "offset": 0,
  "limit": 1000,
  "entries": [
    {"type": "file", "id": "111", "name": "report.pdf", "size": 8192,
     "modified_at": "2025-11-03T00:00:00Z"},
    {"type": "folder", "id": "222", "name": "photos"},
    {"type": "web_link", "id": "333", "name": "shortcut"}
  ]
})";

void test_parse_list()
{
	box::list_result res;
	std::string error;
	check_true("list parses", box::parse_list(list_json, res, error));
	check_true("three entries", res.items.size() == 3);
	if (res.items.size() == 3) {
		check_eq("item 0 name", res.items[0].name, "report.pdf");
		check_eq_int("item 0 size", res.items[0].size, 8192);
		check_true("item 0 not dir", !res.items[0].is_dir);
		check_eq("item 1 id", res.items[1].id, "222");
		check_true("item 1 is dir", res.items[1].is_dir);
		check_eq("item 2 type", res.items[2].type, "web_link");
		check_true("item 2 not dir", !res.items[2].is_dir);
	}
	check_eq_int("total count", res.total_count, 3);
	check_eq_int("offset", res.offset, 0);

	box::list_result empty;
	std::string err2;
	check_true("empty list parses", box::parse_list(R"({"entries":[]})", empty, err2));
	check_true("no items", empty.items.empty());
}

void test_parse_item()
{
	box::item_entry item;
	std::string error;
	check_true("item parses", box::parse_item(R"({"type":"file","id":"9","name":"a.txt","size":3})", item, error));
	check_eq("item id", item.id, "9");
	check_eq("item name", item.name, "a.txt");
	check_eq_int("item size", item.size, 3);

	box::item_entry bad;
	std::string err2;
	check_true("item without id fails", !box::parse_item(R"({"name":"x"})", bad, err2));
}

void test_parse_error()
{
	std::string code, message;
	check_true("error parses", box::parse_error(
		R"({"type":"error","status":404,"code":"not_found","message":"Not Found"})", code, message));
	check_eq("error code", code, "not_found");
	check_eq("error message", message, "Not Found");

	std::string c2, m2;
	check_true("plain doc is not an error", !box::parse_error(R"({"entries":[]})", c2, m2));
}

void test_urls()
{
	check_eq("root folder items", box::folder_items_url("", 0, 1000),
		"https://api.box.com/2.0/folders/0/items?limit=1000&offset=0"
		"&fields=type,id,name,size,modified_at,created_at");
	check_eq("folder items offset", box::folder_items_url("42", 1000, 500),
		"https://api.box.com/2.0/folders/42/items?limit=500&offset=1000"
		"&fields=type,id,name,size,modified_at,created_at");
	check_eq("folder url", box::folder_url("42"), "https://api.box.com/2.0/folders/42");
	check_eq("file url", box::file_url("7"), "https://api.box.com/2.0/files/7");
	check_eq("content url", box::file_content_url("7"), "https://api.box.com/2.0/files/7/content");
	check_eq("new version url", box::new_file_version_url("7"),
		"https://upload.box.com/api/2.0/files/7/content");
}

void test_bodies()
{
	auto const folder = fz::json::parse(box::create_folder_body("newdir", "42"));
	check_eq("folder name", folder["name"].string_value(), "newdir");
	check_eq("folder parent", folder["parent"]["id"].string_value(), "42");

	auto const root_folder = fz::json::parse(box::create_folder_body("newdir", ""));
	check_eq("folder parent defaults to root", root_folder["parent"]["id"].string_value(), "0");

	auto const renamed = fz::json::parse(box::rename_body("b.txt"));
	check_eq("rename name", renamed["name"].string_value(), "b.txt");

	auto const moved = fz::json::parse(box::move_body("99"));
	check_eq("move parent", moved["parent"]["id"].string_value(), "99");
}

}

int main()
{
	test_parse_list();
	test_parse_item();
	test_parse_error();
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
