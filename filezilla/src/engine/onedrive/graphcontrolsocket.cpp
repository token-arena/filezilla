#include "../filezilla.h"

#include "graphcontrolsocket.h"

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

std::string join_path(std::string const& base, std::string const& name)
{
	std::string ret = base;
	if (ret.empty() || ret == "/") {
		ret = "/";
	}
	else if (ret.back() != '/') {
		ret += '/';
	}
	ret += name;
	return ret;
}

}

// ---------------------------------------------------------------------------
// graph_http_client
// ---------------------------------------------------------------------------

graph_http_client::graph_http_client(COneDriveControlSocket & owner)
	: fz::http::client::client(owner, *owner.buffer_pool_, owner.logger(), "FileZilla/"s + ENGINE_VERSION)
	, owner_(owner)
{
}

graph_http_client::~graph_http_client()
{
	destroy();
}

fz::socket_interface* graph_http_client::create_socket(fz::native_string const& host, unsigned short, bool tls)
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

void graph_http_client::destroy_socket()
{
	owner_.ResetSocket();
}

void graph_http_client::on_alive()
{
	owner_.SetAlive();
}

// ---------------------------------------------------------------------------
// COneDriveConnectOpData
// ---------------------------------------------------------------------------

class COneDriveConnectOpData final : public COpData, public COneDriveOpData
{
public:
	explicit COneDriveConnectOpData(COneDriveControlSocket & controlSocket)
		: COpData(Command::connect, L"COneDriveConnectOpData")
		, COneDriveOpData(controlSocket)
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
// COneDriveListOpData
// ---------------------------------------------------------------------------

class COneDriveListOpData final : public COpData, public COneDriveOpData, public CGraphRequestHandler
{
public:
	COneDriveListOpData(COneDriveControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::list, L"COneDriveListOpData")
		, COneDriveOpData(controlSocket)
		, path_(path)
	{
		dir_ = COneDriveControlSocket::PathToString(path_);
	}

	virtual int Send() override
	{
		rr_ = std::make_shared<GraphRequestResponse>();

		std::string url = next_link_.empty() ? graph::item_children_url(dir_, 200) : next_link_;
		if (!controlSocket_.Submit(rr_, url, "GET", {}, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnGraphResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			std::string code, message;
			if (rr_ && graph::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
				log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
			}
			else {
				log(logmsg::error, _("Server returned HTTP status %d"), static_cast<int>(rr_ ? rr_->response_.code_ : 0));
			}
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		graph::list_result result;
		std::string error;
		if (!graph::parse_list(std::string(rr_->response_.body_.to_view()), result, error)) {
			log(logmsg::error, _("Could not parse the server's response: %s"), to_wstring(error));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		for (auto const& item : result.items) {
			if (item.name.empty()) {
				continue;
			}
			CDirentry entry;
			entry.name = to_wstring(item.name);
			entry.size = item.is_dir ? -1 : item.size;
			entry.flags = item.is_dir ? CDirentry::flag_dir : 0;
			if (!item.modified.empty()) {
				entry.time.set_rfc3339(item.modified);
			}
			entries_.push_back(std::move(entry));
		}

		if (!result.next_link.empty() && pages_ < 10000) {
			++pages_;
			next_link_ = result.next_link;
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
	std::shared_ptr<GraphRequestResponse> rr_;
	CServerPath path_;
	std::string dir_;
	std::string next_link_;
	std::vector<CDirentry> entries_;
	int pages_{};
};

// ---------------------------------------------------------------------------
// COneDriveFileTransferOpData
// ---------------------------------------------------------------------------

class COneDriveFileTransferOpData final : public CFileTransferOpData, public COneDriveOpData, public CGraphRequestHandler
{
public:
	COneDriveFileTransferOpData(COneDriveControlSocket & controlSocket, CFileTransferCommand const& cmd)
		: CFileTransferOpData(L"COneDriveFileTransferOpData", cmd)
		, COneDriveOpData(controlSocket)
	{}

	virtual int Send() override
	{
		if (stage_ == 0) {
			dir_ = COneDriveControlSocket::PathToString(remotePath_);
			file_path_ = join_path(dir_, fz::to_utf8(remoteFile_));
			if (download()) {
				int res = controlSocket_.CheckOverwriteFile();
				if (res != FZ_REPLY_OK) {
					return res;
				}
				// First ask for the item to obtain a download URL
				stage_ = 1;
			}
			else {
				if (!reader_factory_) {
					log(logmsg::error, _("No local file to send."));
					return FZ_REPLY_ERROR;
				}
				stage_ = 3;
			}
		}

		rr_ = std::make_shared<GraphRequestResponse>();

		switch (stage_) {
		case 1: // item metadata (to get @microsoft.graph.downloadUrl)
			if (!controlSocket_.Submit(rr_, graph::item_url(file_path_), "GET", {}, {}, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		case 2: // actual download from the pre-authenticated URL
		{
			rr_->set_on_header([this](std::shared_ptr<GraphRequestResponse> const& srr) { return OnHeader(srr); });
			std::map<std::string, std::string> headers;
			if (resume_ && localFileSize_ != 0 && localFileSize_ != fz::aio_base::nosize) {
				headers["Range"] = "bytes=" + std::to_string(localFileSize_) + "-";
			}
			if (!controlSocket_.Submit(rr_, download_url_, "GET", headers, {}, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}
		case 3: // simple upload (PUT .../content)
		{
			rr_->request_.body_ = reader_factory_->open(*controlSocket_.buffer_pool_, 0, fz::aio_base::nosize,
				controlSocket_.max_buffer_count());
			if (!rr_->request_.body_) {
				log(logmsg::error, _("Could not open the local file."));
				return FZ_REPLY_ERROR;
			}
			std::map<std::string, std::string> headers;
			headers["Content-Type"] = "application/octet-stream";
			if (!controlSocket_.Submit(rr_, graph::content_url(file_path_), "PUT", headers, {}, this)) {
				return FZ_REPLY_ERROR;
			}
			rr_->request_.update_content_length_from_body();
			return FZ_REPLY_WOULDBLOCK;
		}
		default:
			return FZ_REPLY_INTERNALERROR;
		}
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnGraphResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			std::string code, message;
			if (rr_ && graph::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
				log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
			}
			else {
				log(logmsg::error, _("File transfer failed"));
			}
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (stage_ == 1) {
			graph::item_entry item;
			std::string error;
			if (!graph::parse_item(std::string(rr_->response_.body_.to_view()), item, error) || item.download_url.empty()) {
				log(logmsg::error, _("Could not obtain a download URL."));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			download_url_ = item.download_url;
			stage_ = 2;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}

		log(logmsg::status, _("File transfer successful"));
		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	fz::http::continuation OnHeader(std::shared_ptr<GraphRequestResponse> const& srr)
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

	std::shared_ptr<GraphRequestResponse> rr_;
	std::string dir_;
	std::string file_path_;
	std::string download_url_;
	int stage_{};
};

// ---------------------------------------------------------------------------
// COneDriveDeleteOpData
// ---------------------------------------------------------------------------

class COneDriveDeleteOpData final : public COpData, public COneDriveOpData, public CGraphRequestHandler
{
public:
	COneDriveDeleteOpData(COneDriveControlSocket & controlSocket, CServerPath const& path, std::vector<std::wstring> files)
		: COpData(Command::del, L"COneDriveDeleteOpData")
		, COneDriveOpData(controlSocket)
		, path_(path)
		, files_(std::move(files))
	{
		dir_ = COneDriveControlSocket::PathToString(path_);
	}

	virtual int Send() override
	{
		if (files_.empty()) {
			return FZ_REPLY_OK;
		}

		rr_ = std::make_shared<GraphRequestResponse>();
		current_ = files_.back();
		std::string const target = join_path(dir_, fz::to_utf8(current_));

		if (!controlSocket_.Submit(rr_, graph::item_url(target), "DELETE", {}, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnGraphResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Deleting file \"%s\" failed"), current_);
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		files_.pop_back();
		rr_.reset();
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
	std::string dir_;
	std::shared_ptr<GraphRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// COneDriveMkdirOpData
// ---------------------------------------------------------------------------

class COneDriveMkdirOpData final : public COpData, public COneDriveOpData, public CGraphRequestHandler
{
public:
	COneDriveMkdirOpData(COneDriveControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::mkdir, L"COneDriveMkdirOpData")
		, COneDriveOpData(controlSocket)
		, path_(path)
	{
		auto full = COneDriveControlSocket::PathToString(path_);
		auto const pos = full.find_last_of('/');
		if (pos == std::string::npos) {
			parent_ = "";
			name_ = full;
		}
		else {
			parent_ = full.substr(0, pos);
			name_ = full.substr(pos + 1);
		}
	}

	virtual int Send() override
	{
		if (name_.empty()) {
			log(logmsg::error, _("Invalid path"));
			return FZ_REPLY_ERROR;
		}

		rr_ = std::make_shared<GraphRequestResponse>();
		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/json; charset=UTF-8";
		if (!controlSocket_.Submit(rr_, graph::create_child_url(parent_), "POST", headers,
				graph::folder_body(name_), this))
		{
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnGraphResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Creating directory failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}
		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	CServerPath path_;
	std::string parent_;
	std::string name_;
	std::shared_ptr<GraphRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// COneDriveRenameOpData
// ---------------------------------------------------------------------------

class COneDriveRenameOpData final : public COpData, public COneDriveOpData, public CGraphRequestHandler
{
public:
	COneDriveRenameOpData(COneDriveControlSocket & controlSocket, CRenameCommand const& command)
		: COpData(Command::rename, L"COneDriveRenameOpData")
		, COneDriveOpData(controlSocket)
		, command_(command)
	{
		from_path_ = join_path(COneDriveControlSocket::PathToString(command_.GetFromPath()),
			fz::to_utf8(command_.GetFromFile()));
		to_dir_ = COneDriveControlSocket::PathToString(command_.GetToPath());
		to_name_ = fz::to_utf8(command_.GetToFile());
		moving_ = to_dir_ != COneDriveControlSocket::PathToString(command_.GetFromPath());
	}

	virtual int Send() override
	{
		rr_ = std::make_shared<GraphRequestResponse>();

		switch (stage_) {
		case 0: // find the source item id
			if (!controlSocket_.Submit(rr_, graph::item_url(from_path_), "GET", {}, {}, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		case 1: // resolve the destination folder id when moving
			if (!controlSocket_.Submit(rr_, graph::item_url(to_dir_), "GET", {}, {}, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		case 2: // patch name (and parent)
		{
			std::map<std::string, std::string> headers;
			headers["Content-Type"] = "application/json; charset=UTF-8";
			std::string const body = moving_ ? graph::move_body(dest_id_, to_name_) : graph::rename_body(to_name_);
			if (!controlSocket_.Submit(rr_, graph::patch_item_url(item_id_), "PATCH", headers, body, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}
		default:
			return FZ_REPLY_INTERNALERROR;
		}
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnGraphResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Renaming file failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());
		rr_.reset();

		if (stage_ == 0) {
			graph::item_entry item;
			std::string error;
			if (!graph::parse_item(body, item, error)) {
				log(logmsg::error, _("File not found"));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			item_id_ = item.id;
			if (moving_) {
				stage_ = 1;
			}
			else {
				stage_ = 2;
			}
			controlSocket_.SendNextCommand();
			return;
		}

		if (stage_ == 1) {
			graph::item_entry item;
			std::string error;
			if (!graph::parse_item(body, item, error)) {
				log(logmsg::error, _("Destination directory not found"));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			dest_id_ = item.id;
			stage_ = 2;
			controlSocket_.SendNextCommand();
			return;
		}

		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	CRenameCommand command_;
	std::string from_path_;
	std::string to_dir_;
	std::string to_name_;
	std::string item_id_;
	std::string dest_id_;
	bool moving_{};
	int stage_{};
	std::shared_ptr<GraphRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// COneDriveControlSocket
// ---------------------------------------------------------------------------

COneDriveControlSocket::COneDriveControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	client_.emplace(*this);
}

COneDriveControlSocket::~COneDriveControlSocket()
{
	client_.reset();
}

void COneDriveControlSocket::ResetSocket()
{
	tls_layer_.reset();
	CRealControlSocket::ResetSocket();
}

void COneDriveControlSocket::Connect(CServer const& server, Credentials const& credentials)
{
	currentServer_ = server;
	credentials_ = credentials;

	token_ = fz::to_utf8(credentials.GetPass());

	// The API host is fixed by the vendor; "api_endpoint" (optionally with its own
	// scheme and port) and "tls" allow reaching a private gateway or an emulator.
	auto const api = fz::to_utf8(server.GetExtraParameter("api_endpoint"));
	has_endpoint_override_ = !api.empty();
	endpoint_ = cloud::resolve(has_endpoint_override_ ? api : std::string("graph.microsoft.com"),
		fz::to_utf8(server.GetExtraParameter("tls")), 443);

	Push(std::make_unique<COneDriveConnectOpData>(*this));
}

std::string COneDriveControlSocket::PathToString(CServerPath const& path)
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

bool COneDriveControlSocket::Submit(std::shared_ptr<GraphRequestResponse> const& rr, std::string const& url,
	std::string const& verb, std::map<std::string, std::string> extra_headers, std::string const& body,
	CGraphRequestHandler * handler)
{
	if (!client_ || !rr) {
		return false;
	}

	auto & req = rr->request_;
	req.verb_ = verb;
	std::string const effective_url = has_endpoint_override_ ? cloud::rebase(url, endpoint_) : url;
	if (!req.uri_.parse(effective_url)) {
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

void COneDriveControlSocket::OnRequestDone(uint64_t, bool success)
{
	auto * handler = pending_handler_;
	pending_handler_ = nullptr;
	if (handler) {
		handler->OnGraphResponse(success);
	}
}

void COneDriveControlSocket::OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info)
{
	if (!tls_layer_ || source != tls_layer_.get()) {
		return;
	}
	SendAsyncRequest(std::make_unique<CCertificateNotification>(std::move(info)));
}

void COneDriveControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::certificate_verification_event, fz::http::client::done_event>(ev, this,
			&COneDriveControlSocket::OnVerifyCert, &COneDriveControlSocket::OnRequestDone))
	{
		return;
	}
	CRealControlSocket::operator()(ev);
}

void COneDriveControlSocket::List(CServerPath const& path, int)
{
	Push(std::make_unique<COneDriveListOpData>(*this, path));
}

void COneDriveControlSocket::FileTransfer(CFileTransferCommand const& cmd)
{
	Push(std::make_unique<COneDriveFileTransferOpData>(*this, cmd));
}

void COneDriveControlSocket::Delete(CServerPath const& path, std::vector<std::wstring>&& files)
{
	Push(std::make_unique<COneDriveDeleteOpData>(*this, path, std::move(files)));
}

void COneDriveControlSocket::RemoveDir(CServerPath const&, std::wstring const&)
{
	log(logmsg::error, _("Removing directories is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

void COneDriveControlSocket::Mkdir(CServerPath const& path, transfer_flags const&)
{
	Push(std::make_unique<COneDriveMkdirOpData>(*this, path));
}

void COneDriveControlSocket::Rename(CRenameCommand const& command)
{
	Push(std::make_unique<COneDriveRenameOpData>(*this, command));
}

void COneDriveControlSocket::Chmod(CChmodCommand const&)
{
	log(logmsg::status, _("Setting permissions is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

int COneDriveControlSocket::DoClose(int nErrorCode)
{
	pending_handler_ = nullptr;
	if (client_) {
		client_->stop(true);
	}
	tls_layer_.reset();

	return CRealControlSocket::DoClose(nErrorCode);
}

bool COneDriveControlSocket::SetAsyncRequestReply(CAsyncRequestNotification * pNotification)
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
