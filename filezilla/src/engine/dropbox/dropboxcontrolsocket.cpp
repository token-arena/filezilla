#include "../filezilla.h"

#include "dropboxcontrolsocket.h"

#include "../controlsocket.h"
#include "../directorycache.h"
#include "../engineprivate.h"
#include "../tls.h"

#include "../../include/directorylisting.h"
#include "../../include/engine_context.h"
#include "../../include/engine_options.h"
#include "../../include/notification.h"

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

// ---------------------------------------------------------------------------
// dropbox_http_client
// ---------------------------------------------------------------------------

dropbox_http_client::dropbox_http_client(CDropboxControlSocket & owner)
	: fz::http::client::client(owner, *owner.buffer_pool_, owner.logger(), "FileZilla/"s + ENGINE_VERSION)
	, owner_(owner)
{
}

dropbox_http_client::~dropbox_http_client()
{
	destroy();
}

fz::socket_interface* dropbox_http_client::create_socket(fz::native_string const& host, unsigned short, bool tls)
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

void dropbox_http_client::destroy_socket()
{
	owner_.ResetSocket();
}

void dropbox_http_client::on_alive()
{
	owner_.SetAlive();
}

// ---------------------------------------------------------------------------
// CDropboxConnectOpData
// ---------------------------------------------------------------------------

class CDropboxConnectOpData final : public COpData, public CDropboxOpData
{
public:
	explicit CDropboxConnectOpData(CDropboxControlSocket & controlSocket)
		: COpData(Command::connect, L"CDropboxConnectOpData")
		, CDropboxOpData(controlSocket)
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
// CDropboxListOpData
// ---------------------------------------------------------------------------

class CDropboxListOpData final : public COpData, public CDropboxOpData, public CDropboxRequestHandler
{
public:
	CDropboxListOpData(CDropboxControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::list, L"CDropboxListOpData")
		, CDropboxOpData(controlSocket)
		, path_(path)
	{
		dir_ = CDropboxControlSocket::PathToString(path_);
	}

	virtual int Send() override
	{
		rr_ = std::make_shared<DropboxRequestResponse>();

		std::string url;
		std::string body;
		if (cursor_.empty()) {
			url = dropbox::rpc_url("files/list_folder");
			body = dropbox::list_body(dir_);
		}
		else {
			url = dropbox::rpc_url("files/list_folder/continue");
			body = dropbox::continue_body(cursor_);
		}

		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/json";

		if (!controlSocket_.Submit(rr_, url, "POST", headers, body, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnDropboxResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			std::string message;
			if (rr_ && dropbox::parse_error(std::string(rr_->response_.body_.to_view()), message)) {
				log(logmsg::error, _("Server error: %s"), to_wstring(message));
			}
			else {
				log(logmsg::error, _("Server returned HTTP status %d"), static_cast<int>(rr_ ? rr_->response_.code_ : 0));
			}
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		dropbox::list_result result;
		std::string error;
		if (!dropbox::parse_list(std::string(rr_->response_.body_.to_view()), result, error)) {
			log(logmsg::error, _("Could not parse the server's response: %s"), to_wstring(error));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		for (auto const& e : result.entries) {
			CDirentry entry;
			entry.name = to_wstring(e.name);
			entry.size = e.is_dir ? -1 : e.size;
			entry.flags = e.is_dir ? CDirentry::flag_dir : 0;
			entries_.push_back(std::move(entry));
		}

		if (result.has_more && !result.cursor.empty() && pages_ < 10000) {
			++pages_;
			cursor_ = result.cursor;
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
	std::shared_ptr<DropboxRequestResponse> rr_;
	CServerPath path_;
	std::string dir_;
	std::string cursor_;
	std::vector<CDirentry> entries_;
	int pages_{};
};

// ---------------------------------------------------------------------------
// CDropboxFileTransferOpData
// ---------------------------------------------------------------------------

class CDropboxFileTransferOpData final : public CFileTransferOpData, public CDropboxOpData, public CDropboxRequestHandler
{
public:
	CDropboxFileTransferOpData(CDropboxControlSocket & controlSocket, CFileTransferCommand const& cmd)
		: CFileTransferOpData(L"CDropboxFileTransferOpData", cmd)
		, CDropboxOpData(controlSocket)
	{}

	virtual int Send() override
	{
		if (stage_ == 0) {
			file_path_ = dropbox::join_path(CDropboxControlSocket::PathToString(remotePath_),
				fz::to_utf8(remoteFile_));

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

		rr_ = std::make_shared<DropboxRequestResponse>();

		std::map<std::string, std::string> headers;
		if (download()) {
			headers["Dropbox-API-Arg"] = dropbox::download_arg(file_path_);
			rr_->set_on_header([this](std::shared_ptr<DropboxRequestResponse> const& srr) { return OnHeader(srr); });
			if (!controlSocket_.Submit(rr_, dropbox::content_url("files/download"), "POST", headers, {}, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		rr_->request_.body_ = reader_factory_->open(*controlSocket_.buffer_pool_, 0, fz::aio_base::nosize,
			controlSocket_.max_buffer_count());
		if (!rr_->request_.body_) {
			log(logmsg::error, _("Could not open the local file."));
			return FZ_REPLY_ERROR;
		}

		headers["Dropbox-API-Arg"] = dropbox::upload_arg(file_path_, true);
		headers["Content-Type"] = "application/octet-stream";
		if (!controlSocket_.Submit(rr_, dropbox::content_url("files/upload"), "POST", headers, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		rr_->request_.update_content_length_from_body();
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnDropboxResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			std::string message;
			if (rr_ && dropbox::parse_error(std::string(rr_->response_.body_.to_view()), message)) {
				log(logmsg::error, _("Server error: %s"), to_wstring(message));
			}
			else {
				log(logmsg::error, _("File transfer failed"));
			}
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		log(logmsg::status, _("File transfer successful"));
		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	fz::http::continuation OnHeader(std::shared_ptr<DropboxRequestResponse> const& srr)
	{
		auto const code = srr->response_.code_;
		if (code != 200 && code != 206) {
			// Error body is collected and reported in OnDropboxResponse
			return fz::http::continuation::done;
		}

		srr->response_.writer_ = controlSocket_.OpenWriter(writer_factory_, 0, true);
		if (!srr->response_.writer_) {
			log(logmsg::error, _("Could not open the local file."));
			return fz::http::continuation::error;
		}
		return fz::http::continuation::next;
	}

	std::shared_ptr<DropboxRequestResponse> rr_;
	std::string file_path_;
	int stage_{};
};

// ---------------------------------------------------------------------------
// CDropboxDeleteOpData
// ---------------------------------------------------------------------------

class CDropboxDeleteOpData final : public COpData, public CDropboxOpData, public CDropboxRequestHandler
{
public:
	CDropboxDeleteOpData(CDropboxControlSocket & controlSocket, CServerPath const& path, std::vector<std::wstring> files)
		: COpData(Command::del, L"CDropboxDeleteOpData")
		, CDropboxOpData(controlSocket)
		, path_(path)
		, files_(std::move(files))
	{
		dir_ = CDropboxControlSocket::PathToString(path_);
	}

	virtual int Send() override
	{
		if (files_.empty()) {
			return FZ_REPLY_OK;
		}

		rr_ = std::make_shared<DropboxRequestResponse>();
		current_ = files_.back();
		std::string const target = dropbox::join_path(dir_, fz::to_utf8(current_));

		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/json";

		if (!controlSocket_.Submit(rr_, dropbox::rpc_url("files/delete_v2"), "POST", headers,
				dropbox::delete_body(target), this))
		{
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnDropboxResponse(bool success) override
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
	std::shared_ptr<DropboxRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CDropboxMkdirOpData
// ---------------------------------------------------------------------------

class CDropboxMkdirOpData final : public COpData, public CDropboxOpData, public CDropboxRequestHandler
{
public:
	CDropboxMkdirOpData(CDropboxControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::mkdir, L"CDropboxMkdirOpData")
		, CDropboxOpData(controlSocket)
		, path_(path)
	{
		dir_ = CDropboxControlSocket::PathToString(path_);
	}

	virtual int Send() override
	{
		if (dir_.empty()) {
			log(logmsg::error, _("Invalid path"));
			return FZ_REPLY_ERROR;
		}

		rr_ = std::make_shared<DropboxRequestResponse>();
		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/json";

		if (!controlSocket_.Submit(rr_, dropbox::rpc_url("files/create_folder_v2"), "POST", headers,
				dropbox::create_folder_body(dir_), this))
		{
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnDropboxResponse(bool success) override
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
	std::string dir_;
	std::shared_ptr<DropboxRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CDropboxRenameOpData
// ---------------------------------------------------------------------------

class CDropboxRenameOpData final : public COpData, public CDropboxOpData, public CDropboxRequestHandler
{
public:
	CDropboxRenameOpData(CDropboxControlSocket & controlSocket, CRenameCommand const& command)
		: COpData(Command::rename, L"CDropboxRenameOpData")
		, CDropboxOpData(controlSocket)
		, command_(command)
	{
		from_ = dropbox::join_path(CDropboxControlSocket::PathToString(command_.GetFromPath()),
			fz::to_utf8(command_.GetFromFile()));
		to_ = dropbox::join_path(CDropboxControlSocket::PathToString(command_.GetToPath()),
			fz::to_utf8(command_.GetToFile()));
	}

	virtual int Send() override
	{
		rr_ = std::make_shared<DropboxRequestResponse>();
		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/json";

		if (!controlSocket_.Submit(rr_, dropbox::rpc_url("files/move_v2"), "POST", headers,
				dropbox::move_body(from_, to_), this))
		{
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnDropboxResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Renaming file failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}
		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	CRenameCommand command_;
	std::string from_;
	std::string to_;
	std::shared_ptr<DropboxRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CDropboxControlSocket
// ---------------------------------------------------------------------------

CDropboxControlSocket::CDropboxControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	client_.emplace(*this);
}

CDropboxControlSocket::~CDropboxControlSocket()
{
	client_.reset();
}

void CDropboxControlSocket::ResetSocket()
{
	tls_layer_.reset();
	CRealControlSocket::ResetSocket();
}

void CDropboxControlSocket::Connect(CServer const& server, Credentials const& credentials)
{
	currentServer_ = server;
	credentials_ = credentials;

	token_ = fz::to_utf8(credentials.GetPass());

	// The API host is fixed by the vendor; "api_endpoint" (optionally with its own
	// scheme and port) and "tls" allow reaching a private gateway or an emulator.
	auto const api = fz::to_utf8(server.GetExtraParameter("api_endpoint"));
	has_endpoint_override_ = !api.empty();
	endpoint_ = cloud::resolve(has_endpoint_override_ ? api : std::string("api.dropboxapi.com"),
		fz::to_utf8(server.GetExtraParameter("tls")), 443);

	Push(std::make_unique<CDropboxConnectOpData>(*this));
}

std::string CDropboxControlSocket::PathToString(CServerPath const& path)
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

bool CDropboxControlSocket::Submit(std::shared_ptr<DropboxRequestResponse> const& rr, std::string const& url,
	std::string const& verb, std::map<std::string, std::string> extra_headers, std::string const& body,
	CDropboxRequestHandler * handler)
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
	else if (!req.body_ && !req.headers_.count("Content-Length")) {
		req.headers_["Content-Length"] = "0";
	}

	pending_handler_ = handler;
	if (!client_->add_request(rr)) {
		pending_handler_ = nullptr;
		return false;
	}
	return true;
}

void CDropboxControlSocket::OnRequestDone(uint64_t, bool success)
{
	auto * handler = pending_handler_;
	pending_handler_ = nullptr;
	if (handler) {
		handler->OnDropboxResponse(success);
	}
}

void CDropboxControlSocket::OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info)
{
	if (!tls_layer_ || source != tls_layer_.get()) {
		return;
	}
	SendAsyncRequest(std::make_unique<CCertificateNotification>(std::move(info)));
}

void CDropboxControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::certificate_verification_event, fz::http::client::done_event>(ev, this,
			&CDropboxControlSocket::OnVerifyCert, &CDropboxControlSocket::OnRequestDone))
	{
		return;
	}
	CRealControlSocket::operator()(ev);
}

void CDropboxControlSocket::List(CServerPath const& path, int)
{
	Push(std::make_unique<CDropboxListOpData>(*this, path));
}

void CDropboxControlSocket::FileTransfer(CFileTransferCommand const& cmd)
{
	Push(std::make_unique<CDropboxFileTransferOpData>(*this, cmd));
}

void CDropboxControlSocket::Delete(CServerPath const& path, std::vector<std::wstring>&& files)
{
	Push(std::make_unique<CDropboxDeleteOpData>(*this, path, std::move(files)));
}

void CDropboxControlSocket::RemoveDir(CServerPath const&, std::wstring const&)
{
	log(logmsg::error, _("Removing directories is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

void CDropboxControlSocket::Mkdir(CServerPath const& path, transfer_flags const&)
{
	Push(std::make_unique<CDropboxMkdirOpData>(*this, path));
}

void CDropboxControlSocket::Rename(CRenameCommand const& command)
{
	Push(std::make_unique<CDropboxRenameOpData>(*this, command));
}

void CDropboxControlSocket::Chmod(CChmodCommand const&)
{
	log(logmsg::status, _("Setting permissions is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

int CDropboxControlSocket::DoClose(int nErrorCode)
{
	pending_handler_ = nullptr;
	if (client_) {
		client_->stop(true);
	}
	tls_layer_.reset();

	return CRealControlSocket::DoClose(nErrorCode);
}

bool CDropboxControlSocket::SetAsyncRequestReply(CAsyncRequestNotification * pNotification)
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
