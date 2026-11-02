#include "s3response.h"

#if defined(HAVE_SYSTEM_PUGIXML) && HAVE_SYSTEM_PUGIXML
#include <pugixml.hpp>
#else
#include "../../pugixml/pugixml.hpp"
#endif

#include <cstdlib>

namespace s3 {

namespace {

std::string child_text(pugi::xml_node const& parent, char const* name)
{
	auto const child = parent.child(name);
	if (!child) {
		return {};
	}
	return child.child_value();
}

int64_t child_int(pugi::xml_node const& parent, char const* name, int64_t fallback = 0)
{
	auto const child = parent.child(name);
	if (!child) {
		return fallback;
	}
	char const* value = child.child_value();
	if (!value || !*value) {
		return fallback;
	}
	return std::strtoll(value, nullptr, 10);
}

bool child_bool(pugi::xml_node const& parent, char const* name, bool fallback = false)
{
	auto const text = child_text(parent, name);
	if (text.empty()) {
		return fallback;
	}
	return text == "true" || text == "True" || text == "1";
}

bool load(std::string_view const& xml, pugi::xml_document & doc)
{
	if (xml.empty()) {
		return false;
	}
	// load_buffer does not expect a NUL terminator.
	auto const result = doc.load_buffer(xml.data(), xml.size(), pugi::parse_default | pugi::parse_ws_pcdata);
	return static_cast<bool>(result);
}

}

bool parse_list_objects(std::string_view const& xml, list_result & out)
{
	out = list_result();

	pugi::xml_document doc;
	if (!load(xml, doc)) {
		return false;
	}

	auto const root = doc.child("ListBucketResult");
	if (!root && !doc.child("ListVersionsResult")) {
		return false;
	}

	auto container = root ? root : doc.child("ListVersionsResult");

	out.bucket = child_text(container, "Name");
	out.prefix = child_text(container, "Prefix");
	out.delimiter = child_text(container, "Delimiter");
	out.continuation_token = child_text(container, "ContinuationToken");
	out.max_keys = child_int(container, "MaxKeys", 1000);
	out.truncated = child_bool(container, "IsTruncated", false);

	out.next_continuation_token = child_text(container, "NextContinuationToken");
	if (out.next_continuation_token.empty()) {
		out.next_continuation_token = child_text(container, "NextMarker");
	}

	for (auto const & node : container.children("Contents")) {
		object_entry entry;
		entry.key = child_text(node, "Key");
		if (entry.key.empty()) {
			continue;
		}
		entry.size = child_int(node, "Size", 0);
		entry.etag = child_text(node, "ETag");
		entry.last_modified = child_text(node, "LastModified");
		entry.storage_class = child_text(node, "StorageClass");
		entry.is_prefix = false;
		out.entries.push_back(std::move(entry));
	}

	for (auto const & node : container.children("CommonPrefixes")) {
		object_entry entry;
		entry.key = child_text(node, "Prefix");
		if (entry.key.empty()) {
			continue;
		}
		entry.is_prefix = true;
		out.entries.push_back(std::move(entry));
	}

	return true;
}

bool parse_error(std::string_view const& xml, error_info & out)
{
	out = error_info();

	pugi::xml_document doc;
	if (!load(xml, doc)) {
		return false;
	}

	auto const root = doc.child("Error");
	if (!root) {
		return false;
	}

	out.code = child_text(root, "Code");
	out.message = child_text(root, "Message");
	out.request_id = child_text(root, "RequestId");
	out.resource = child_text(root, "Resource");

	return true;
}

bool parse_list_buckets(std::string_view const& xml, std::vector<bucket_entry> & out)
{
	out.clear();

	pugi::xml_document doc;
	if (!load(xml, doc)) {
		return false;
	}

	auto const root = doc.child("ListAllMyBucketsResult");
	if (!root) {
		return false;
	}

	for (auto const & node : root.child("Buckets").children("Bucket")) {
		bucket_entry entry;
		entry.name = child_text(node, "Name");
		if (entry.name.empty()) {
			continue;
		}
		entry.creation_date = child_text(node, "CreationDate");
		out.push_back(std::move(entry));
	}

	return true;
}
}
