#include "../filezilla.h"

#include "drivecontrolsocket.h"

#include "../controlsocket.h"
#include "../directorycache.h"
#include "../engineprivate.h"
#include "../tls.h"

#include "../../include/directorylisting.h"
#include "../../include/engine_context.h"
#include "../../include/engine_options.h"
#include "../../include/notification.h"

#include <libfilezilla/json.hpp>
#include <libfilezilla/tls_layer.hpp>
#include <libfilezilla/tls_system_trust_store.hpp>

#include <algorithm>

using namespace std::literals;

namespace {

std::wstring to_wstring(std::string const& in)
{
	return fz::to_wstring_from_utf8(in);
}

}

/// Walks a path segment by segment, remembering the Drive folder id reached so far.
struct drive_resolver
{
	std::vector<std::string> segments;
	size_t index{};
	std::string current_id{"root"};
	std::string current_path;

	bool done() const { return index >= segments.size(); }

	std::string const& next_segment() const { return segments[index]; }

	std::string const& next_path()
	{
		pending_path_ = current_path.empty() ? segments[index] : current_path + "/" + segments[index];
		return pending_path_;
	}

	void accept(std::string const& id)
	{
		current_id = id;
		current_path = pending_path_;
		++index;
	}

private:
	std::string pending_path_;
};

// ---------------------------------------------------------------------------
// drive_http_client
// ---------------------------------------------------------------------------

drive_http_client::drive_http_client(CGDriveControlSocket & owner)
	: fz::http::client::client(owner, *owner.buffer_pool_, owner.logger(), "FileZilla/"s + ENGINE_VERSION)
	, owner_(owner)
{
}

drive_http_client::~drive_http_client()
{
	destroy();
}

fz::socket_interface* drive_http_client::create_socket(fz::native_string const& host, unsigned short, bool tls)
{
#if FZ_WINDOWS
	owner_.CreateSocket(host);
#else
	owner_.CreateSocket(fz::to_wstring_from_utf8(host));
#endif

	if (tls) {
		owner_.tls_layer_ = std::make_unique<fz::tls_layer>(owner_.event_loop_, nullptr, *owner_.active_layer_,
			&owner_.engine_.GetContext().GetTlsSystemTrustStore(), owner_.logger_);
		owner_.active_layer_ = owner_.tls_layer_.get();

		owner_.tls_layer_->set_alpn("http/1.1");
		owner_.tls_layer_->set_min_tls_ver(get_min_tls_ver(owner_.engine_.GetOptions()));
		if (!owner_.tls_layer_->client_handshake(&owner_)) {
			owner_.ResetSocket();
			return nullptr;
		}
	}

	return owner_.active_layer_;
}

void drive_http_client::destroy_socket()
{
	owner_.ResetSocket();
}

void drive_http_client::on_alive()
{
	owner_.SetAlive();
}

// ---------------------------------------------------------------------------
// CDriveConnectOpData
// ---------------------------------------------------------------------------

class CDriveConnectOpData final : public COpData, public CDriveOpData
{
public:
	explicit CDriveConnectOpData(CGDriveControlSocket & controlSocket)
		: COpData(Command::connect, L"CDriveConnectOpData")
		, CDriveOpData(controlSocket)
	{}

	virtual int Send() override
	{
		if (controlSocket_.token_.empty()) {
			log(logmsg::error, _("An access token is required as password."));
			return FZ_REPLY_CRITICALERROR;
		}
		return FZ_REPLY_OK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }
};

// ---------------------------------------------------------------------------
// CDriveListOpData
// ---------------------------------------------------------------------------

class CDriveListOpData final : public COpData, public CDriveOpData, public CDriveRequestHandler
{
public:
	CDriveListOpData(CGDriveControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::list, L"CDriveListOpData")
		, CDriveOpData(controlSocket)
		, path_(path)
	{
		resolver_.segments = CGDriveControlSocket::SplitPath(path_);
	}

	virtual int Send() override
	{
		for (;;) {
			if (stage_ == 0) {
				if (resolver_.done()) {
					folder_id_ = resolver_.current_id;
					stage_ = 1;
					continue;
				}

				auto const& next_path = resolver_.next_path();
				if (auto const cached = controlSocket_.CachedId(next_path); !cached.empty()) {
					resolver_.accept(cached);
					continue;
				}

				rr_ = std::make_shared<DriveRequestResponse>();
				if (!controlSocket_.Submit(rr_, gdrive::search_url(
						gdrive::folder_query(resolver_.current_id, resolver_.next_segment())),
						"GET", {}, {}, this))
				{
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}

			rr_ = std::make_shared<DriveRequestResponse>();
			if (!controlSocket_.Submit(rr_, gdrive::list_url(folder_id_, page_token_), "GET", {}, {}, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnDriveResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log_error();
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());

		if (stage_ == 0) {
			std::vector<gdrive::file_entry> found;
			std::string token, error;
			if (!gdrive::parse_list_files(body, found, token, error) || found.empty()) {
				log(logmsg::error, _("Directory \"%s\" not found"), to_wstring(resolver_.next_path()));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			controlSocket_.CacheId(resolver_.next_path(), found.front().id);
			resolver_.accept(found.front().id);
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}

		std::vector<gdrive::file_entry> files;
		std::string token, error;
		if (!gdrive::parse_list_files(body, files, token, error)) {
			log(logmsg::error, _("Could not parse the server's response: %s"), to_wstring(error));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		for (auto const& f : files) {
			CDirentry entry;
			entry.name = to_wstring(f.name);
			entry.size = f.is_dir ? -1 : f.size;
			entry.flags = f.is_dir ? CDirentry::flag_dir : 0;
			if (!f.modified_time.empty()) {
				entry.time.set_rfc3339(f.modified_time);
			}
			entries_.push_back(std::move(entry));
		}

		if (!token.empty() && pages_ < 10000) {
			++pages_;
			page_token_ = token;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}

		CDirectoryListing listing;
		listing.path = path_;
		listing.m_firstListTime = fz::monotonic_clock::now();
		for (auto & e : entries_) {
			listing.Append(std::move(e));
		}
		engine_.GetDirectoryCache().Store(listing, currentServer_);
		controlSocket_.SendDirectoryListingNotification(path_, false);
		log(logmsg::status, _("Directory listing successful"));
		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	void log_error()
	{
		std::string message;
		if (rr_ && gdrive::parse_error(std::string(rr_->response_.body_.to_view()), message)) {
			log(logmsg::error, _("Server error: %s"), to_wstring(message));
		}
		else {
			log(logmsg::error, _("Server returned HTTP status %d"), static_cast<int>(rr_ ? rr_->response_.code_ : 0));
		}
	}

	std::shared_ptr<DriveRequestResponse> rr_;
	CServerPath path_;
	drive_resolver resolver_;
	std::string folder_id_;
	std::string page_token_;
	std::vector<CDirentry> entries_;
	int stage_{};
	int pages_{};
};

// ---------------------------------------------------------------------------
// CDriveFileTransferOpData
// ---------------------------------------------------------------------------

class CDriveFileTransferOpData final : public CFileTransferOpData, public CDriveOpData, public CDriveRequestHandler
{
public:
	CDriveFileTransferOpData(CGDriveControlSocket & controlSocket, CFileTransferCommand const& cmd)
		: CFileTransferOpData(L"CDriveFileTransferOpData", cmd)
		, CDriveOpData(controlSocket)
	{}

	virtual int Send() override
	{
		if (stage_ == 0) {
			resolver_.segments = CGDriveControlSocket::SplitPath(remotePath_);
			file_name_ = fz::to_utf8(remoteFile_);
			if (download()) {
				int res = controlSocket_.CheckOverwriteFile();
				if (res != FZ_REPLY_OK) {
					return res;
				}
			}
			else if (!reader_factory_) {
				log(logmsg::error, _("No local file to send."));
				return FZ_REPLY_ERROR;
			}
			stage_ = 1;
		}

		for (;;) {
			switch (stage_) {
			case 1: // resolve the parent folder
			{
				if (resolver_.done()) {
					parent_id_ = resolver_.current_id;
					stage_ = download() ? 2 : 4;
					continue;
				}

				auto const& next_path = resolver_.next_path();
				if (auto const cached = controlSocket_.CachedId(next_path); !cached.empty()) {
					resolver_.accept(cached);
					continue;
				}

				rr_ = std::make_shared<DriveRequestResponse>();
				if (!controlSocket_.Submit(rr_, gdrive::search_url(
						gdrive::folder_query(resolver_.current_id, resolver_.next_segment())),
						"GET", {}, {}, this))
				{
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 2: // look up the file id
			{
				rr_ = std::make_shared<DriveRequestResponse>();
				std::string query = "'" + gdrive::escape_query_value(parent_id_) + "' in parents and name = '" +
					gdrive::escape_query_value(file_name_) + "' and trashed = false";
				if (!controlSocket_.Submit(rr_, gdrive::search_url(query), "GET", {}, {}, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 3: // download
			{
				rr_ = std::make_shared<DriveRequestResponse>();
				rr_->set_on_header([this](std::shared_ptr<DriveRequestResponse> const& srr) { return OnHeader(srr); });
				std::map<std::string, std::string> headers;
				if (resume_ && localFileSize_ != 0 && localFileSize_ != fz::aio_base::nosize) {
					headers["Range"] = "bytes=" + std::to_string(localFileSize_) + "-";
				}
				if (!controlSocket_.Submit(rr_, gdrive::media_url(file_id_), "GET", headers, {}, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 4: // create empty file metadata
			{
				rr_ = std::make_shared<DriveRequestResponse>();
				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/json; charset=UTF-8";
				if (!controlSocket_.Submit(rr_, gdrive::create_url(), "POST", headers,
						gdrive::create_file_body(file_name_, parent_id_), this))
				{
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 5: // upload the content
			{
				rr_ = std::make_shared<DriveRequestResponse>();
				rr_->request_.body_ = reader_factory_->open(*controlSocket_.buffer_pool_, 0, fz::aio_base::nosize,
					controlSocket_.max_buffer_count());
				if (!rr_->request_.body_) {
					log(logmsg::error, _("Could not open the local file."));
					return FZ_REPLY_ERROR;
				}
				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/octet-stream";
				if (!controlSocket_.Submit(rr_, gdrive::upload_url(file_id_), "PATCH", headers, {}, this)) {
					return FZ_REPLY_ERROR;
				}
				rr_->request_.update_content_length_from_body();
				return FZ_REPLY_WOULDBLOCK;
			}
			default:
				return FZ_REPLY_INTERNALERROR;
			}
		}
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnDriveResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			std::string message;
			if (rr_ && gdrive::parse_error(std::string(rr_->response_.body_.to_view()), message)) {
				log(logmsg::error, _("Server error: %s"), to_wstring(message));
			}
			else {
				log(logmsg::error, _("Request failed"));
			}
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());

		switch (stage_) {
		case 1:
		{
			std::vector<gdrive::file_entry> found;
			std::string token, error;
			if (!gdrive::parse_list_files(body, found, token, error) || found.empty()) {
				log(logmsg::error, _("Directory \"%s\" not found"), to_wstring(resolver_.next_path()));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			controlSocket_.CacheId(resolver_.next_path(), found.front().id);
			resolver_.accept(found.front().id);
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}
		case 2:
		{
			std::vector<gdrive::file_entry> found;
			std::string token, error;
			if (!gdrive::parse_list_files(body, found, token, error)) {
				log(logmsg::error, _("Could not parse the server's response."));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			if (found.empty()) {
				log(logmsg::error, _("File \"%s\" not found"), to_wstring(file_name_));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			file_id_ = found.front().id;
			stage_ = 3;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}
		case 4:
		{
			auto const doc = fz::json::parse(body);
			file_id_ = doc["id"].string_value();
			if (file_id_.empty()) {
				log(logmsg::error, _("Could not create the file."));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			stage_ = 5;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}
		default:
			log(logmsg::status, _("File transfer successful"));
			controlSocket_.ResetOperation(FZ_REPLY_OK);
			return;
		}
	}

private:
	fz::http::continuation OnHeader(std::shared_ptr<DriveRequestResponse> const& srr)
	{
		auto const code = srr->response_.code_;
		if (code == 416 && resume_) {
			resume_ = false;
			return fz::http::continuation::error;
		}
		if (code != 200 && code != 206) {
			return fz::http::continuation::done;
		}

		uint64_t offset = 0;
		if (resume_ && localFileSize_ != 0 && localFileSize_ != fz::aio_base::nosize) {
			offset = localFileSize_;
		}
		srr->response_.writer_ = controlSocket_.OpenWriter(writer_factory_, offset, true);
		if (!srr->response_.writer_) {
			log(logmsg::error, _("Could not open the local file."));
			return fz::http::continuation::error;
		}
		return fz::http::continuation::next;
	}

	std::shared_ptr<DriveRequestResponse> rr_;
	drive_resolver resolver_;
	std::string file_name_;
	std::string parent_id_;
	std::string file_id_;
	int stage_{};
};

// ---------------------------------------------------------------------------
// CDriveDeleteOpData
// ---------------------------------------------------------------------------

class CDriveDeleteOpData final : public COpData, public CDriveOpData, public CDriveRequestHandler
{
public:
	CDriveDeleteOpData(CGDriveControlSocket & controlSocket, CServerPath const& path, std::vector<std::wstring> files)
		: COpData(Command::del, L"CDriveDeleteOpData")
		, CDriveOpData(controlSocket)
		, path_(path)
		, files_(std::move(files))
	{
		resolver_.segments = CGDriveControlSocket::SplitPath(path_);
	}

	virtual int Send() override
	{
		for (;;) {
			switch (stage_) {
			case 0:
				if (files_.empty()) {
					return FZ_REPLY_OK;
				}
				stage_ = 1;
				continue;
			case 1: // resolve parent folder
			{
				if (resolver_.done()) {
					parent_id_ = resolver_.current_id;
					current_ = files_.back();
					key_ = fz::to_utf8(current_);
					stage_ = 2;
					continue;
				}
				auto const& next_path = resolver_.next_path();
				if (auto const cached = controlSocket_.CachedId(next_path); !cached.empty()) {
					resolver_.accept(cached);
					continue;
				}
				rr_ = std::make_shared<DriveRequestResponse>();
				if (!controlSocket_.Submit(rr_, gdrive::search_url(
						gdrive::folder_query(resolver_.current_id, resolver_.next_segment())),
						"GET", {}, {}, this))
				{
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 2: // find the file
			{
				rr_ = std::make_shared<DriveRequestResponse>();
				std::string query = "'" + gdrive::escape_query_value(parent_id_) + "' in parents and name = '" +
					gdrive::escape_query_value(key_) + "' and trashed = false";
				if (!controlSocket_.Submit(rr_, gdrive::search_url(query), "GET", {}, {}, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 3: // delete
			{
				rr_ = std::make_shared<DriveRequestResponse>();
				if (!controlSocket_.Submit(rr_, gdrive::delete_url(file_id_), "DELETE", {}, {}, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			default:
				return FZ_REPLY_INTERNALERROR;
			}
		}
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnDriveResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Deleting file \"%s\" failed"), current_);
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());
		rr_.reset();

		if (stage_ == 1) {
			std::vector<gdrive::file_entry> found;
			std::string token, error;
			if (!gdrive::parse_list_files(body, found, token, error) || found.empty()) {
				log(logmsg::error, _("Directory not found"));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			controlSocket_.CacheId(resolver_.next_path(), found.front().id);
			resolver_.accept(found.front().id);
			controlSocket_.SendNextCommand();
			return;
		}

		if (stage_ == 2) {
			std::vector<gdrive::file_entry> found;
			std::string token, error;
			if (!gdrive::parse_list_files(body, found, token, error) || found.empty()) {
				log(logmsg::error, _("File \"%s\" not found"), current_);
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			file_id_ = found.front().id;
			stage_ = 3;
			controlSocket_.SendNextCommand();
			return;
		}

		// deletion finished, continue with the next file
		files_.pop_back();
		stage_ = 1;
		if (files_.empty()) {
			controlSocket_.ResetOperation(FZ_REPLY_OK);
			return;
		}
		controlSocket_.SendNextCommand();
	}

private:
	CServerPath path_;
	std::vector<std::wstring> files_;
	std::wstring current_;
	std::string parent_id_;
	std::string key_;
	std::string file_id_;
	drive_resolver resolver_;
	int stage_{};
	std::shared_ptr<DriveRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CDriveMkdirOpData
// ---------------------------------------------------------------------------

class CDriveMkdirOpData final : public COpData, public CDriveOpData, public CDriveRequestHandler
{
public:
	CDriveMkdirOpData(CGDriveControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::mkdir, L"CDriveMkdirOpData")
		, CDriveOpData(controlSocket)
		, path_(path)
	{
		resolver_.segments = CGDriveControlSocket::SplitPath(path_);
		if (resolver_.segments.empty()) {
			invalid_ = true;
		}
		else {
			dir_name_ = resolver_.segments.back();
			resolver_.segments.pop_back();
		}
	}

	virtual int Send() override
	{
		if (invalid_) {
			log(logmsg::error, _("Invalid path"));
			return FZ_REPLY_ERROR;
		}

		for (;;) {
			if (stage_ == 0) {
				if (resolver_.done()) {
					parent_id_ = resolver_.current_id;
					stage_ = 1;
					continue;
				}
				auto const& next_path = resolver_.next_path();
				if (auto const cached = controlSocket_.CachedId(next_path); !cached.empty()) {
					resolver_.accept(cached);
					continue;
				}
				rr_ = std::make_shared<DriveRequestResponse>();
				if (!controlSocket_.Submit(rr_, gdrive::search_url(
						gdrive::folder_query(resolver_.current_id, resolver_.next_segment())),
						"GET", {}, {}, this))
				{
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}

			rr_ = std::make_shared<DriveRequestResponse>();
			std::map<std::string, std::string> headers;
			headers["Content-Type"] = "application/json; charset=UTF-8";
			if (!controlSocket_.Submit(rr_, gdrive::create_url(), "POST", headers,
					gdrive::create_folder_body(dir_name_, parent_id_), this))
			{
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnDriveResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Creating directory failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());
		rr_.reset();

		if (stage_ == 0) {
			std::vector<gdrive::file_entry> found;
			std::string token, error;
			if (!gdrive::parse_list_files(body, found, token, error) || found.empty()) {
				log(logmsg::error, _("Parent directory not found"));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			controlSocket_.CacheId(resolver_.next_path(), found.front().id);
			resolver_.accept(found.front().id);
			controlSocket_.SendNextCommand();
			return;
		}

		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	CServerPath path_;
	std::string dir_name_;
	std::string parent_id_;
	drive_resolver resolver_;
	int stage_{};
	bool invalid_{};
	std::shared_ptr<DriveRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CDriveRenameOpData
// ---------------------------------------------------------------------------

class CDriveRenameOpData final : public COpData, public CDriveOpData, public CDriveRequestHandler
{
public:
	CDriveRenameOpData(CGDriveControlSocket & controlSocket, CRenameCommand const& command)
		: COpData(Command::rename, L"CDriveRenameOpData")
		, CDriveOpData(controlSocket)
		, command_(command)
	{
		from_ = CGDriveControlSocket::SplitPath(command_.GetFromPath());
		to_ = CGDriveControlSocket::SplitPath(command_.GetToPath());
		from_name_ = fz::to_utf8(command_.GetFromFile());
		to_name_ = fz::to_utf8(command_.GetToFile());
		if (from_ == to_) {
			to_.clear(); // simple rename inside the same folder
		}
	}

	virtual int Send() override
	{
		for (;;) {
			switch (stage_) {
			case 0: // resolve source folder and find the file
			{
				auto & res = (*resolver_);
				if (res.done()) {
					from_id_ = res.current_id;
					stage_ = 1;
					continue;
				}
				auto const& next_path = res.next_path();
				if (auto const cached = controlSocket_.CachedId(next_path); !cached.empty()) {
					res.accept(cached);
					continue;
				}
				rr_ = std::make_shared<DriveRequestResponse>();
				if (!controlSocket_.Submit(rr_, gdrive::search_url(
						gdrive::folder_query(res.current_id, res.next_segment())), "GET", {}, {}, this))
				{
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 1: // search the file
			{
				rr_ = std::make_shared<DriveRequestResponse>();
				std::string query = "'" + gdrive::escape_query_value(from_id_) + "' in parents and name = '" +
					gdrive::escape_query_value(from_name_) + "' and trashed = false";
				if (!controlSocket_.Submit(rr_, gdrive::search_url(query), "GET", {}, {}, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 2: // resolve destination folder (if moving)
			{
				if (resolver_->done()) {
					to_id_ = resolver_->current_id;
					stage_ = 3;
					continue;
				}
				auto const& next_path = resolver_->next_path();
				if (auto const cached = controlSocket_.CachedId(next_path); !cached.empty()) {
					resolver_->accept(cached);
					continue;
				}
				rr_ = std::make_shared<DriveRequestResponse>();
				if (!controlSocket_.Submit(rr_, gdrive::search_url(
						gdrive::folder_query(resolver_->current_id, resolver_->next_segment())), "GET", {}, {}, this))
				{
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 3: // patch name (and parents when moving)
			{
				rr_ = std::make_shared<DriveRequestResponse>();
				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/json; charset=UTF-8";

				std::string url = gdrive::patch_url(file_id_);
				std::string body;
				if (to_.empty()) {
					body = gdrive::rename_body(to_name_);
				}
				else {
					body = gdrive::move_body(to_id_, from_id_);
					url += "?addParents=" + gdrive::encode_component(to_id_) +
						"&removeParents=" + gdrive::encode_component(from_id_);
				}
				if (!controlSocket_.Submit(rr_, url, "PATCH", headers, body, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			default:
				return FZ_REPLY_INTERNALERROR;
			}
		}
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnDriveResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Renaming file failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());
		rr_.reset();

		if (stage_ == 0) {
			std::vector<gdrive::file_entry> found;
			std::string token, error;
			if (!gdrive::parse_list_files(body, found, token, error) || found.empty()) {
				log(logmsg::error, _("Directory not found"));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			controlSocket_.CacheId(resolver_->next_path(), found.front().id);
			resolver_->accept(found.front().id);
			controlSocket_.SendNextCommand();
			return;
		}

		if (stage_ == 1) {
			std::vector<gdrive::file_entry> found;
			std::string token, error;
			if (!gdrive::parse_list_files(body, found, token, error) || found.empty()) {
				log(logmsg::error, _("File not found"));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			file_id_ = found.front().id;
			stage_ = 2;
			controlSocket_.SendNextCommand();
			return;
		}

		if (stage_ == 2) {
			std::vector<gdrive::file_entry> found;
			std::string token, error;
			if (!gdrive::parse_list_files(body, found, token, error) || found.empty()) {
				log(logmsg::error, _("Destination directory not found"));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			controlSocket_.CacheId(resolver_->next_path(), found.front().id);
			resolver_->accept(found.front().id);
			controlSocket_.SendNextCommand();
			return;
		}

		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	CRenameCommand command_;
	std::vector<std::string> from_;
	std::vector<std::string> to_;
	std::string from_name_;
	std::string to_name_;
	std::string from_id_;
	std::string to_id_;
	std::string file_id_;
	drive_resolver from_resolver_;
	drive_resolver to_resolver_;
	drive_resolver * resolver_{&from_resolver_};
	int stage_{};
	std::shared_ptr<DriveRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CGDriveControlSocket
// ---------------------------------------------------------------------------

CGDriveControlSocket::CGDriveControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	client_.emplace(*this);
}

CGDriveControlSocket::~CGDriveControlSocket()
{
	client_.reset();
}

void CGDriveControlSocket::ResetSocket()
{
	tls_layer_.reset();
	CRealControlSocket::ResetSocket();
}

void CGDriveControlSocket::Connect(CServer const& server, Credentials const& credentials)
{
	currentServer_ = server;
	credentials_ = credentials;

	token_ = fz::to_utf8(credentials.GetPass());
	id_cache_[""] = "root";

	Push(std::make_unique<CDriveConnectOpData>(*this));
}

std::vector<std::string> CGDriveControlSocket::SplitPath(CServerPath const& path)
{
	std::vector<std::string> ret;

	std::string full = fz::to_utf8(path.GetPath());
	while (!full.empty() && full.front() == '/') {
		full.erase(0, 1);
	}

	size_t start = 0;
	while (start < full.size()) {
		auto pos = full.find('/', start);
		if (pos == std::string::npos) {
			pos = full.size();
		}
		if (pos > start) {
			ret.push_back(full.substr(start, pos - start));
		}
		start = pos + 1;
	}
	return ret;
}

std::string CGDriveControlSocket::CachedId(std::string const& path) const
{
	auto const it = id_cache_.find(path);
	if (it == id_cache_.end()) {
		return {};
	}
	return it->second;
}

void CGDriveControlSocket::CacheId(std::string const& path, std::string const& id)
{
	if (!path.empty() && !id.empty()) {
		id_cache_[path] = id;
	}
}

bool CGDriveControlSocket::Submit(std::shared_ptr<DriveRequestResponse> const& rr, std::string const& url,
	std::string const& verb, std::map<std::string, std::string> extra_headers, std::string const& body,
	CDriveRequestHandler * handler)
{
	if (!client_ || !rr) {
		return false;
	}

	auto & req = rr->request_;
	req.verb_ = verb;
	if (!req.uri_.parse(url)) {
		log(logmsg::error, L"Could not parse URL: %s", to_wstring(url));
		return false;
	}

	for (auto const& h : extra_headers) {
		req.headers_[h.first] = h.second;
	}
	if (!token_.empty()) {
		req.headers_["Authorization"] = "Bearer " + token_;
	}

	if (!body.empty()) {
		req.body_ = fz::string_reader_factory(L"json", body).open(*buffer_pool_);
		if (!req.body_) {
			return false;
		}
		req.update_content_length_from_body();
	}
	else if (!req.body_ && (verb == "POST" || verb == "PUT" || verb == "PATCH")) {
		req.headers_["Content-Length"] = "0";
	}

	pending_handler_ = handler;
	if (!client_->add_request(rr)) {
		pending_handler_ = nullptr;
		return false;
	}
	return true;
}

void CGDriveControlSocket::OnRequestDone(uint64_t, bool success)
{
	auto * handler = pending_handler_;
	pending_handler_ = nullptr;
	if (handler) {
		handler->OnDriveResponse(success);
	}
}

void CGDriveControlSocket::OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info)
{
	if (!tls_layer_ || source != tls_layer_.get()) {
		return;
	}
	SendAsyncRequest(std::make_unique<CCertificateNotification>(std::move(info)));
}

void CGDriveControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::certificate_verification_event, fz::http::client::done_event>(ev, this,
			&CGDriveControlSocket::OnVerifyCert, &CGDriveControlSocket::OnRequestDone))
	{
		return;
	}
	CRealControlSocket::operator()(ev);
}

void CGDriveControlSocket::List(CServerPath const& path, int)
{
	Push(std::make_unique<CDriveListOpData>(*this, path));
}

void CGDriveControlSocket::FileTransfer(CFileTransferCommand const& cmd)
{
	Push(std::make_unique<CDriveFileTransferOpData>(*this, cmd));
}

void CGDriveControlSocket::Delete(CServerPath const& path, std::vector<std::wstring>&& files)
{
	Push(std::make_unique<CDriveDeleteOpData>(*this, path, std::move(files)));
}

void CGDriveControlSocket::RemoveDir(CServerPath const&, std::wstring const&)
{
	log(logmsg::error, _("Removing directories is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

void CGDriveControlSocket::Mkdir(CServerPath const& path, transfer_flags const&)
{
	Push(std::make_unique<CDriveMkdirOpData>(*this, path));
}

void CGDriveControlSocket::Rename(CRenameCommand const& command)
{
	Push(std::make_unique<CDriveRenameOpData>(*this, command));
}

void CGDriveControlSocket::Chmod(CChmodCommand const&)
{
	log(logmsg::status, _("Setting permissions is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

int CGDriveControlSocket::DoClose(int nErrorCode)
{
	pending_handler_ = nullptr;
	if (client_) {
		client_->stop(true);
	}
	tls_layer_.reset();

	return CRealControlSocket::DoClose(nErrorCode);
}

bool CGDriveControlSocket::SetAsyncRequestReply(CAsyncRequestNotification * pNotification)
{
	if (pNotification->GetRequestID() == reqId_certificate) {
		auto const * notification = dynamic_cast<CCertificateNotification const*>(pNotification);
		if (!notification || !tls_layer_) {
			ResetOperation(FZ_REPLY_INTERNALERROR);
			return false;
		}
		if (!notification->trusted_) {
			log(logmsg::error, _("The server's certificate could not be verified."));
			ResetOperation(FZ_REPLY_ERROR);
			return false;
		}
		return true;
	}
	return false;
}
