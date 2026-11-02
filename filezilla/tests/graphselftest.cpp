#include "../src/engine/onedrive/graphjson.h"

#include <libfilezilla/json.hpp>

#include <cstdio>
#include <string>

// Standalone self-test for the Microsoft Graph (OneDrive) logic layer.
// Wired into `make check` as graphselftest.

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
  "value": [
    {"id": "01", "name": "report.pdf", "size": 2048,
     "file": {"mimeType": "application/pdf"},
     "lastModifiedDateTime": "2025-11-03T00:00:00Z"},
    {"id": "02", "name": "photos", "folder": {"childCount": 3},
     "lastModifiedDateTime": "2025-11-03T00:00:00Z"},
    {"id": "03", "name": "shared.docx", "size": 10,
     "@microsoft.graph.downloadUrl": "https://example.com/dl"}
  ],
  "@odata.nextLink": "https://graph.microsoft.com/v1.0/me/drive/root/children?$skiptoken=abc"
})";

void test_parse_list()
{
	graph::list_result res;
	std::string error;
	check_true("list parses", graph::parse_list(list_json, res, error));
	check_true("three items", res.items.size() == 3);
	if (res.items.size() == 3) {
		check_eq("item 0 name", res.items[0].name, "report.pdf");
		check_eq_int("item 0 size", res.items[0].size, 2048);
		check_eq("item 0 mime", res.items[0].mime_type, "application/pdf");
		check_true("item 0 is not a dir", !res.items[0].is_dir);
		check_true("item 1 is a dir", res.items[1].is_dir);
		check_eq("item 2 download url", res.items[2].download_url, "https://example.com/dl");
	}
	check_eq("next link", res.next_link,
		"https://graph.microsoft.com/v1.0/me/drive/root/children?$skiptoken=abc");

	graph::list_result empty;
	std::string err2;
	check_true("empty list parses", graph::parse_list(R"({"value":[]})", empty, err2));
	check_true("no items", empty.items.empty());
	check_true("no next link", empty.next_link.empty());
}

void test_parse_item()
{
	graph::item_entry item;
	std::string error;
	check_true("item parses", graph::parse_item(
		R"({"id":"05","name":"a.txt","size":7,"file":{"mimeType":"text/plain"}})", item, error));
	check_eq("item id", item.id, "05");
	check_eq("item name", item.name, "a.txt");
	check_eq_int("item size", item.size, 7);
	check_true("item not a dir", !item.is_dir);

	graph::item_entry bad;
	std::string err2;
	check_true("garbage item fails", !graph::parse_item("nope", bad, err2));
}

void test_parse_error()
{
	std::string code, message;
	check_true("error parses", graph::parse_error(
		R"({"error":{"code":"itemNotFound","message":"The resource could not be found."}})", code, message));
	check_eq("error code", code, "itemNotFound");
	check_eq("error message", message, "The resource could not be found.");

	std::string c2, m2;
	check_true("plain doc is not an error", !graph::parse_error(R"({"value":[]})", c2, m2));
}

void test_urls()
{
	check_eq("root children", graph::root_children_url(200),
		"https://graph.microsoft.com/v1.0/me/drive/root/children?$top=200");
	check_eq("children by path", graph::item_children_url("/docs/2025", 200),
		"https://graph.microsoft.com/v1.0/me/drive/root:/docs/2025:/children?$top=200");
	check_eq("item by path", graph::item_url("/docs/a b.txt"),
		"https://graph.microsoft.com/v1.0/me/drive/root:/docs/a%20b.txt:");
	check_eq("content url", graph::content_url("/a.txt"),
		"https://graph.microsoft.com/v1.0/me/drive/root:/a.txt:/content");
	check_eq("create child", graph::create_child_url("/docs"),
		"https://graph.microsoft.com/v1.0/me/drive/root:/docs:/children");
	check_eq("create child root", graph::create_child_url(""),
		"https://graph.microsoft.com/v1.0/me/drive/root/children");
	check_eq("patch url", graph::patch_item_url("01ABC"),
		"https://graph.microsoft.com/v1.0/me/drive/items/01ABC");
	check_eq("encode keeps slashes", graph::encode_path("/a/b c"), "/a/b%20c");
}

void test_bodies()
{
	auto const folder = fz::json::parse(graph::folder_body("newdir"));
	check_eq("folder name", folder["name"].string_value(), "newdir");
	check_true("folder marker present", folder["folder"].is_object());
	check_eq("conflict behaviour", folder["@microsoft.graph.conflictBehavior"].string_value(), "rename");

	auto const renamed = fz::json::parse(graph::rename_body("b.txt"));
	check_eq("rename name", renamed["name"].string_value(), "b.txt");

	auto const moved = fz::json::parse(graph::move_body("parentId", "new.txt"));
	check_eq("move parent", moved["parentReference"]["id"].string_value(), "parentId");
	check_eq("move name", moved["name"].string_value(), "new.txt");
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
