#include "../filezilla.h"

#include "boxcontrolsocket.h"

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

/// Walks a path segment by segment, remembering the Box folder id reached so far.
struct box_resolver
{
	std::vector<std::string> segments;
	size_t index{};
	std::string current_id{"0"};
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
// box_http_client
// ---------------------------------------------------------------------------

box_http_client::box_http_client(CBoxControlSocket & owner)
	: fz::http::client::client(owner, *owner.buffer_pool_, owner.logger(), "FileZilla/"s + ENGINE_VERSION)
	, owner_(owner)
{
}

box_http_client::~box_http_client()
{
	destroy();
}

fz::socket_interface* box_http_client::create_socket(fz::native_string const& host, unsigned short, bool tls)
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

void box_http_client::destroy_socket()
{
	owner_.ResetSocket();
}

void box_http_client::on_alive()
{
	owner_.SetAlive();
}

// ---------------------------------------------------------------------------
// CBoxConnectOpData
// ---------------------------------------------------------------------------

class CBoxConnectOpData final : public COpData, public CBoxOpData
{
public:
	explicit CBoxConnectOpData(CBoxControlSocket & controlSocket)
		: COpData(Command::connect, L"CBoxConnectOpData")
		, CBoxOpData(controlSocket)
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
// CBoxListOpData
// ---------------------------------------------------------------------------

class CBoxListOpData final : public COpData, public CBoxOpData, public CBoxRequestHandler
{
public:
	CBoxListOpData(CBoxControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::list, L"CBoxListOpData")
		, CBoxOpData(controlSocket)
		, path_(path)
	{
		resolver_.segments = CBoxControlSocket::SplitPath(path_);
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
				rr_ = std::make_shared<BoxRequestResponse>();
				if (!controlSocket_.Submit(rr_, box::folder_items_url(resolver_.current_id, 0, 1000),
						"GET", {}, {}, this))
				{
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}

			rr_ = std::make_shared<BoxRequestResponse>();
			if (!controlSocket_.Submit(rr_, box::folder_items_url(folder_id_, offset_, 1000), "GET", {}, {}, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnBoxResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log_error();
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		box::list_result result;
		std::string error;
		if (!box::parse_list(std::string(rr_->response_.body_.to_view()), result, error)) {
			log(logmsg::error, _("Could not parse the server's response: %s"), to_wstring(error));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (stage_ == 0) {
			// Looking for the next path segment
			std::string const& wanted = resolver_.next_segment();
			for (auto const& item : result.items) {
				if (item.is_dir && item.name == wanted) {
					controlSocket_.CacheId(resolver_.next_path(), item.id);
					resolver_.accept(item.id);
					rr_.reset();
					controlSocket_.SendNextCommand();
					return;
				}
			}
			log(logmsg::error, _("Directory \"%s\" not found"), to_wstring(wanted));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		for (auto const& item : result.items) {
			CDirentry entry;
			entry.name = to_wstring(item.name);
			entry.size = item.is_dir ? -1 : item.size;
			entry.flags = item.is_dir ? CDirentry::flag_dir : 0;
			entries_.push_back(std::move(entry));
		}

		offset_ += static_cast<int64_t>(result.items.size());
		if (!result.items.empty() && result.total_count > 0 && offset_ < result.total_count && pages_ < 10000) {
			++pages_;
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
		std::string code, message;
		if (rr_ && box::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
			log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
		}
		else {
			log(logmsg::error, _("Server returned HTTP status %d"), static_cast<int>(rr_ ? rr_->response_.code_ : 0));
		}
	}

	std::shared_ptr<BoxRequestResponse> rr_;
	CServerPath path_;
	box_resolver resolver_;
	std::string folder_id_;
	int64_t offset_{};
	std::vector<CDirentry> entries_;
	int stage_{};
	int pages_{};
};

// ---------------------------------------------------------------------------
// CBoxFileTransferOpData
// ---------------------------------------------------------------------------

class CBoxFileTransferOpData final : public CFileTransferOpData, public CBoxOpData, public CBoxRequestHandler
{
public:
	CBoxFileTransferOpData(CBoxControlSocket & controlSocket, CFileTransferCommand const& cmd)
		: CFileTransferOpData(L"CBoxFileTransferOpData", cmd)
		, CBoxOpData(controlSocket)
	{}

	virtual int Send() override
	{
		if (stage_ == 0) {
			resolver_.segments = CBoxControlSocket::SplitPath(remotePath_);
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
			case 1: // resolve parent folder
			{
				if (resolver_.done()) {
					parent_id_ = resolver_.current_id;
					stage_ = 2;
					continue;
				}
				auto const& next_path = resolver_.next_path();
				if (auto const cached = controlSocket_.CachedId(next_path); !cached.empty()) {
					resolver_.accept(cached);
					continue;
				}
				rr_ = std::make_shared<BoxRequestResponse>();
				if (!controlSocket_.Submit(rr_, box::folder_items_url(resolver_.current_id, 0, 1000),
						"GET", {}, {}, this))
				{
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 2: // find an entry with the same name
			{
				rr_ = std::make_shared<BoxRequestResponse>();
				if (!controlSocket_.Submit(rr_, box::folder_items_url(parent_id_, 0, 1000), "GET", {}, {}, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 3: // download: content request (expects a redirect)
			{
				rr_ = std::make_shared<BoxRequestResponse>();
				rr_->set_on_header([this](std::shared_ptr<BoxRequestResponse> const& srr) { return OnContentHeader(srr); });
				if (!controlSocket_.Submit(rr_, box::file_content_url(file_id_), "GET", {}, {}, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 4: // follow the pre-authenticated redirect
			{
				rr_ = std::make_shared<BoxRequestResponse>();
				rr_->set_on_header([this](std::shared_ptr<BoxRequestResponse> const& srr) { return OnContentHeader(srr); });
				if (!controlSocket_.Submit(rr_, redirect_url_, "GET", {}, {}, this, false)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 5: // upload a new version of the existing file
			{
				rr_ = std::make_shared<BoxRequestResponse>();
				rr_->request_.body_ = reader_factory_->open(*controlSocket_.buffer_pool_, 0, fz::aio_base::nosize,
					controlSocket_.max_buffer_count());
				if (!rr_->request_.body_) {
					log(logmsg::error, _("Could not open the local file."));
					return FZ_REPLY_ERROR;
				}
				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/octet-stream";
				if (!controlSocket_.Submit(rr_, box::new_file_version_url(file_id_), "POST", headers, {}, this)) {
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

	virtual void OnBoxResponse(bool success) override
	{
		if (!rr_) {
			controlSocket_.ResetOperation(FZ_REPLY_INTERNALERROR);
			return;
		}

		if (stage_ == 1 || stage_ == 2 || stage_ == 3) {
			if (!success || !rr_->response_.success()) {
				std::string code, message;
				if (box::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
					log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
				}
				else {
					log(logmsg::error, _("Request failed"));
				}
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
		}

		std::string const body(rr_->response_.body_.to_view());

		switch (stage_) {
		case 1:
		{
			box::list_result result;
			std::string error;
			if (!box::parse_list(body, result, error)) {
				log(logmsg::error, _("Could not parse the server's response."));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			std::string const& wanted = resolver_.next_segment();
			for (auto const& item : result.items) {
				if (item.is_dir && item.name == wanted) {
					controlSocket_.CacheId(resolver_.next_path(), item.id);
					resolver_.accept(item.id);
					rr_.reset();
					controlSocket_.SendNextCommand();
					return;
				}
			}
			log(logmsg::error, _("Directory \"%s\" not found"), to_wstring(wanted));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}
		case 2:
		{
			box::list_result result;
			std::string error;
			if (!box::parse_list(body, result, error)) {
				log(logmsg::error, _("Could not parse the server's response."));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}

			for (auto const& item : result.items) {
				if (!item.is_dir && item.name == file_name_) {
					file_id_ = item.id;
					break;
				}
			}

			if (download()) {
				if (file_id_.empty()) {
					log(logmsg::error, _("File \"%s\" not found"), to_wstring(file_name_));
					controlSocket_.ResetOperation(FZ_REPLY_ERROR);
					return;
				}
				stage_ = 3;
			}
			else {
				if (file_id_.empty()) {
					// Creating a new file requires a multipart upload, which the
					// streaming reader cannot express yet.
					log(logmsg::error, _("Uploading new files to Box is not supported yet, upload to an existing file name instead."));
					controlSocket_.ResetOperation(FZ_REPLY_NOTSUPPORTED);
					return;
				}
				stage_ = 5;
			}
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}
		case 3:
		{
			// A redirect was expected, handled in OnContentHeader
			auto const code = rr_->response_.code_;
			if (code == 200 || code == 206) {
				// No redirect, the body already went into the writer
				log(logmsg::status, _("File transfer successful"));
				controlSocket_.ResetOperation(FZ_REPLY_OK);
				return;
			}
			log(logmsg::error, _("Server returned HTTP status %d"), static_cast<int>(code));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}
		case 4:
			if (!success || !rr_->response_.success()) {
				log(logmsg::error, _("Download failed"));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			log(logmsg::status, _("File transfer successful"));
			controlSocket_.ResetOperation(FZ_REPLY_OK);
			return;
		default:
			if (!success || !rr_->response_.success()) {
				log(logmsg::error, _("File transfer failed"));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			log(logmsg::status, _("File transfer successful"));
			controlSocket_.ResetOperation(FZ_REPLY_OK);
			return;
		}
	}

private:
	fz::http::continuation OnContentHeader(std::shared_ptr<BoxRequestResponse> const& srr)
	{
		auto const code = srr->response_.code_;

		if (code >= 300 && code < 400) {
			if (auto const location = srr->response_.get_header("location"); !location.empty()) {
				redirect_url_ = location;
				if (redirect_url_.rfind("http", 0) != 0) {
					// Relative redirect
					redirect_url_ = "https://api.box.com" + redirect_url_;
				}
				return fz::http::continuation::done;
			}
			return fz::http::continuation::error;
		}

		if (code != 200 && code != 206) {
			return fz::http::continuation::done;
		}

		srr->response_.writer_ = controlSocket_.OpenWriter(writer_factory_, 0, true);
		if (!srr->response_.writer_) {
			log(logmsg::error, _("Could not open the local file."));
			return fz::http::continuation::error;
		}
		return fz::http::continuation::next;
	}

	std::shared_ptr<BoxRequestResponse> rr_;
	box_resolver resolver_;
	std::string file_name_;
	std::string parent_id_;
	std::string file_id_;
	std::string redirect_url_;
	int stage_{};
};

// ---------------------------------------------------------------------------
// CBoxDeleteOpData
// ---------------------------------------------------------------------------

class CBoxDeleteOpData final : public COpData, public CBoxOpData, public CBoxRequestHandler
{
public:
	CBoxDeleteOpData(CBoxControlSocket & controlSocket, CServerPath const& path, std::vector<std::wstring> files)
		: COpData(Command::del, L"CBoxDeleteOpData")
		, CBoxOpData(controlSocket)
		, path_(path)
		, files_(std::move(files))
	{
		resolver_.segments = CBoxControlSocket::SplitPath(path_);
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
			case 1:
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
				rr_ = std::make_shared<BoxRequestResponse>();
				if (!controlSocket_.Submit(rr_, box::folder_items_url(resolver_.current_id, 0, 1000),
						"GET", {}, {}, this))
				{
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 2:
			{
				rr_ = std::make_shared<BoxRequestResponse>();
				if (!controlSocket_.Submit(rr_, box::folder_items_url(parent_id_, 0, 1000), "GET", {}, {}, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case 3:
			{
				rr_ = std::make_shared<BoxRequestResponse>();
				if (!controlSocket_.Submit(rr_, box::file_url(file_id_), "DELETE", {}, {}, this)) {
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

	virtual void OnBoxResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Deleting file \"%s\" failed"), current_);
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());
		rr_.reset();

		if (stage_ == 1) {
			box::list_result result;
			std::string error;
			if (!box::parse_list(body, result, error)) {
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			std::string const& wanted = resolver_.next_segment();
			for (auto const& item : result.items) {
				if (item.is_dir && item.name == wanted) {
					controlSocket_.CacheId(resolver_.next_path(), item.id);
					resolver_.accept(item.id);
					controlSocket_.SendNextCommand();
					return;
				}
			}
			log(logmsg::error, _("Directory not found"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (stage_ == 2) {
			box::list_result result;
			std::string error;
			if (!box::parse_list(body, result, error)) {
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			for (auto const& item : result.items) {
				if (!item.is_dir && item.name == key_) {
					file_id_ = item.id;
					break;
				}
			}
			if (file_id_.empty()) {
				log(logmsg::error, _("File \"%s\" not found"), current_);
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			stage_ = 3;
			controlSocket_.SendNextCommand();
			return;
		}

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
	std::string key_;
	std::string parent_id_;
	std::string file_id_;
	box_resolver resolver_;
	int stage_{};
	std::shared_ptr<BoxRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CBoxMkdirOpData
// ---------------------------------------------------------------------------

class CBoxMkdirOpData final : public COpData, public CBoxOpData, public CBoxRequestHandler
{
public:
	CBoxMkdirOpData(CBoxControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::mkdir, L"CBoxMkdirOpData")
		, CBoxOpData(controlSocket)
		, path_(path)
	{
		resolver_.segments = CBoxControlSocket::SplitPath(path_);
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
				rr_ = std::make_shared<BoxRequestResponse>();
				if (!controlSocket_.Submit(rr_, box::folder_items_url(resolver_.current_id, 0, 1000),
						"GET", {}, {}, this))
				{
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}

			rr_ = std::make_shared<BoxRequestResponse>();
			std::map<std::string, std::string> headers;
			headers["Content-Type"] = "application/json";
			if (!controlSocket_.Submit(rr_, "https://api.box.com/2.0/folders", "POST", headers,
					box::create_folder_body(dir_name_, parent_id_), this))
			{
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnBoxResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Creating directory failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());
		rr_.reset();

		if (stage_ == 0) {
			box::list_result result;
			std::string error;
			if (!box::parse_list(body, result, error)) {
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			std::string const& wanted = resolver_.next_segment();
			for (auto const& item : result.items) {
				if (item.is_dir && item.name == wanted) {
					controlSocket_.CacheId(resolver_.next_path(), item.id);
					resolver_.accept(item.id);
					controlSocket_.SendNextCommand();
					return;
				}
			}
			log(logmsg::error, _("Parent directory not found"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	CServerPath path_;
	std::string dir_name_;
	std::string parent_id_;
	box_resolver resolver_;
	int stage_{};
	bool invalid_{};
	std::shared_ptr<BoxRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CBoxRenameOpData
// ---------------------------------------------------------------------------

class CBoxRenameOpData final : public COpData, public CBoxOpData, public CBoxRequestHandler
{
public:
	CBoxRenameOpData(CBoxControlSocket & controlSocket, CRenameCommand const& command)
		: COpData(Command::rename, L"CBoxRenameOpData")
		, CBoxOpData(controlSocket)
		, command_(command)
	{
		from_resolver_.segments = CBoxControlSocket::SplitPath(command_.GetFromPath());
		to_resolver_.segments = CBoxControlSocket::SplitPath(command_.GetToPath());
		from_name_ = fz::to_utf8(command_.GetFromFile());
		to_name_ = fz::to_utf8(command_.GetToFile());
		moving_ = CBoxControlSocket::PathToString(command_.GetFromPath()) !=
			CBoxControlSocket::PathToString(command_.GetToPath());
	}

	virtual int Send() override
	{
		rr_ = std::make_shared<BoxRequestResponse>();

		if (stage_ == 0) {
			if (from_resolver_.done()) {
				from_parent_ = from_resolver_.current_id;
				stage_ = 1;
				controlSocket_.SendNextCommand();
				return FZ_REPLY_WOULDBLOCK;
			}
			auto const& next_path = from_resolver_.next_path();
			if (auto const cached = controlSocket_.CachedId(next_path); !cached.empty()) {
				from_resolver_.accept(cached);
				controlSocket_.SendNextCommand();
				return FZ_REPLY_WOULDBLOCK;
			}
			if (!controlSocket_.Submit(rr_, box::folder_items_url(from_resolver_.current_id, 0, 1000),
					"GET", {}, {}, this))
			{
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		if (stage_ == 1) {
			if (!controlSocket_.Submit(rr_, box::folder_items_url(from_parent_, 0, 1000), "GET", {}, {}, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		if (stage_ == 2 && moving_) {
			if (to_resolver_.done()) {
				to_parent_ = to_resolver_.current_id;
				stage_ = 3;
				controlSocket_.SendNextCommand();
				return FZ_REPLY_WOULDBLOCK;
			}
			auto const& next_path = to_resolver_.next_path();
			if (auto const cached = controlSocket_.CachedId(next_path); !cached.empty()) {
				to_resolver_.accept(cached);
				controlSocket_.SendNextCommand();
				return FZ_REPLY_WOULDBLOCK;
			}
			if (!controlSocket_.Submit(rr_, box::folder_items_url(to_resolver_.current_id, 0, 1000),
					"GET", {}, {}, this))
			{
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		// Final patch
		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/json";
		std::string body = box::rename_body(to_name_);
		if (moving_) {
			auto moved = fz::json::parse(box::move_body(to_parent_));
			auto renamed = fz::json::parse(box::rename_body(to_name_));
			moved["name"] = renamed["name"];
			body = moved.to_string();
		}

		std::string const url = item_is_dir_ ? box::folder_url(item_id_) : box::file_url(item_id_);
		if (!controlSocket_.Submit(rr_, url, "PUT", headers, body, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnBoxResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Renaming file failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());
		rr_.reset();

		if (stage_ == 0) {
			box::list_result result;
			std::string error;
			if (!box::parse_list(body, result, error)) {
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			std::string const& wanted = from_resolver_.next_segment();
			for (auto const& item : result.items) {
				if (item.is_dir && item.name == wanted) {
					controlSocket_.CacheId(from_resolver_.next_path(), item.id);
					from_resolver_.accept(item.id);
					controlSocket_.SendNextCommand();
					return;
				}
			}
			log(logmsg::error, _("Directory not found"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (stage_ == 1) {
			box::list_result result;
			std::string error;
			if (!box::parse_list(body, result, error)) {
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			for (auto const& item : result.items) {
				if (item.name == from_name_) {
					item_id_ = item.id;
					item_is_dir_ = item.is_dir;
					break;
				}
			}
			if (item_id_.empty()) {
				log(logmsg::error, _("File not found"));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			stage_ = 2;
			controlSocket_.SendNextCommand();
			return;
		}

		if (stage_ == 2 && moving_) {
			box::list_result result;
			std::string error;
			if (!box::parse_list(body, result, error)) {
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			std::string const& wanted = to_resolver_.next_segment();
			for (auto const& item : result.items) {
				if (item.is_dir && item.name == wanted) {
					controlSocket_.CacheId(to_resolver_.next_path(), item.id);
					to_resolver_.accept(item.id);
					controlSocket_.SendNextCommand();
					return;
				}
			}
			log(logmsg::error, _("Destination directory not found"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	CRenameCommand command_;
	box_resolver from_resolver_;
	box_resolver to_resolver_;
	std::string from_name_;
	std::string to_name_;
	std::string from_parent_;
	std::string to_parent_;
	std::string item_id_;
	bool moving_{};
	bool item_is_dir_{};
	int stage_{};
	std::shared_ptr<BoxRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CBoxControlSocket
// ---------------------------------------------------------------------------

CBoxControlSocket::CBoxControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	client_.emplace(*this);
}

CBoxControlSocket::~CBoxControlSocket()
{
	client_.reset();
}

void CBoxControlSocket::ResetSocket()
{
	tls_layer_.reset();
	CRealControlSocket::ResetSocket();
}

void CBoxControlSocket::Connect(CServer const& server, Credentials const& credentials)
{
	currentServer_ = server;
	credentials_ = credentials;

	token_ = fz::to_utf8(credentials.GetPass());
	id_cache_[""] = box::root_folder_id;

	Push(std::make_unique<CBoxConnectOpData>(*this));
}

std::vector<std::string> CBoxControlSocket::SplitPath(CServerPath const& path)
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

std::string CBoxControlSocket::PathToString(CServerPath const& path)
{
	std::string full = fz::to_utf8(path.GetPath());
	if (full == "/") {
		full.clear();
	}
	while (!full.empty() && full.back() == '/') {
		full.pop_back();
	}
	return full;
}

std::string CBoxControlSocket::CachedId(std::string const& path) const
{
	auto const it = id_cache_.find(path);
	if (it == id_cache_.end()) {
		return {};
	}
	return it->second;
}

void CBoxControlSocket::CacheId(std::string const& path, std::string const& id)
{
	if (!path.empty() && !id.empty()) {
		id_cache_[path] = id;
	}
}

bool CBoxControlSocket::Submit(std::shared_ptr<BoxRequestResponse> const& rr, std::string const& url,
	std::string const& verb, std::map<std::string, std::string> extra_headers, std::string const& body,
	CBoxRequestHandler * handler, bool with_auth)
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
	if (with_auth && !token_.empty()) {
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

void CBoxControlSocket::OnRequestDone(uint64_t, bool success)
{
	auto * handler = pending_handler_;
	pending_handler_ = nullptr;
	if (handler) {
		handler->OnBoxResponse(success);
	}
}

void CBoxControlSocket::OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info)
{
	if (!tls_layer_ || source != tls_layer_.get()) {
		return;
	}
	SendAsyncRequest(std::make_unique<CCertificateNotification>(std::move(info)));
}

void CBoxControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::certificate_verification_event, fz::http::client::done_event>(ev, this,
			&CBoxControlSocket::OnVerifyCert, &CBoxControlSocket::OnRequestDone))
	{
		return;
	}
	CRealControlSocket::operator()(ev);
}

void CBoxControlSocket::List(CServerPath const& path, int)
{
	Push(std::make_unique<CBoxListOpData>(*this, path));
}

void CBoxControlSocket::FileTransfer(CFileTransferCommand const& cmd)
{
	Push(std::make_unique<CBoxFileTransferOpData>(*this, cmd));
}

void CBoxControlSocket::Delete(CServerPath const& path, std::vector<std::wstring>&& files)
{
	Push(std::make_unique<CBoxDeleteOpData>(*this, path, std::move(files)));
}

void CBoxControlSocket::RemoveDir(CServerPath const&, std::wstring const&)
{
	log(logmsg::error, _("Removing directories is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

void CBoxControlSocket::Mkdir(CServerPath const& path, transfer_flags const&)
{
	Push(std::make_unique<CBoxMkdirOpData>(*this, path));
}

void CBoxControlSocket::Rename(CRenameCommand const& command)
{
	Push(std::make_unique<CBoxRenameOpData>(*this, command));
}

void CBoxControlSocket::Chmod(CChmodCommand const&)
{
	log(logmsg::status, _("Setting permissions is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

int CBoxControlSocket::DoClose(int nErrorCode)
{
	pending_handler_ = nullptr;
	if (client_) {
		client_->stop(true);
	}
	tls_layer_.reset();

	return CRealControlSocket::DoClose(nErrorCode);
}

bool CBoxControlSocket::SetAsyncRequestReply(CAsyncRequestNotification * pNotification)
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
