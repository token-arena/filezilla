#include "../filezilla.h"

#include "gcscontrolsocket.h"

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

std::wstring name_from_key(std::string const& key, std::string const& prefix, bool & is_dir)
{
	std::string name = key;
	if (!prefix.empty() && name.compare(0, prefix.size(), prefix) == 0) {
		name = name.substr(prefix.size());
	}
	is_dir = false;
	if (!name.empty() && name.back() == '/') {
		name.pop_back();
		is_dir = true;
	}
	return to_wstring(name);
}

}

// ---------------------------------------------------------------------------
// gcs_http_client
// ---------------------------------------------------------------------------

gcs_http_client::gcs_http_client(CGcsControlSocket & owner)
	: fz::http::client::client(owner, *owner.buffer_pool_, owner.logger(), "FileZilla/"s + ENGINE_VERSION)
	, owner_(owner)
{
}

gcs_http_client::~gcs_http_client()
{
	destroy();
}

fz::socket_interface* gcs_http_client::create_socket(fz::native_string const& host, unsigned short, bool tls)
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

void gcs_http_client::destroy_socket()
{
	owner_.ResetSocket();
}

void gcs_http_client::on_alive()
{
	owner_.SetAlive();
}

// ---------------------------------------------------------------------------
// CGcsConnectOpData
// ---------------------------------------------------------------------------

class CGcsConnectOpData final : public COpData, public CGcsOpData
{
public:
	explicit CGcsConnectOpData(CGcsControlSocket & controlSocket)
		: COpData(Command::connect, L"CGcsConnectOpData")
		, CGcsOpData(controlSocket)
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
// CGcsListOpData
// ---------------------------------------------------------------------------

class CGcsListOpData final : public COpData, public CGcsOpData, public CGcsRequestHandler
{
public:
	CGcsListOpData(CGcsControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::list, L"CGcsListOpData")
		, CGcsOpData(controlSocket)
		, path_(path)
	{
		auto split = CGcsControlSocket::SplitPath(path_);
		bucket_ = split.first;
		prefix_ = split.second;
	}

	virtual int Send() override
	{
		rr_ = std::make_shared<GcsRequestResponse>();

		std::string url;
		if (bucket_.empty()) {
			if (controlSocket_.project_.empty()) {
				log(logmsg::error, _("Listing buckets requires a project id, set the \"project\" parameter of this site."));
				return FZ_REPLY_ERROR;
			}
			url = "https://storage.googleapis.com/storage/v1/b?project=" + gcs::encode_object_name(controlSocket_.project_);
			if (!page_token_.empty()) {
				url += "&pageToken=" + gcs::encode_object_name(page_token_);
			}
		}
		else {
			url = gcs::list_objects_url(bucket_, prefix_, true, page_token_);
		}

		if (!controlSocket_.Submit(rr_, url, "GET", {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnGcsResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log_error();
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());

		if (bucket_.empty()) {
			std::vector<gcs::bucket_entry> buckets;
			std::string error;
			if (!gcs::parse_list_buckets(body, buckets, error)) {
				log(logmsg::error, _("Could not parse the server's response: %s"), to_wstring(error));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			for (auto const& b : buckets) {
				CDirentry entry;
				entry.name = to_wstring(b.name);
				entry.size = -1;
				entry.flags = CDirentry::flag_dir;
				entries_.push_back(std::move(entry));
			}
			Finish(false);
			return;
		}

		gcs::list_objects_result result;
		std::string error;
		if (!gcs::parse_list_objects(body, result, error)) {
			log(logmsg::error, _("Could not parse the server's response: %s"), to_wstring(error));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		for (auto const& o : result.objects) {
			bool is_dir = o.is_prefix;
			std::wstring const name = name_from_key(o.name, prefix_, is_dir);
			if (name.empty()) {
				continue;
			}
			CDirentry entry;
			entry.name = name;
			entry.size = is_dir ? -1 : o.size;
			entry.flags = is_dir ? CDirentry::flag_dir : 0;
			if (!o.updated.empty()) {
				entry.time.set_rfc3339(o.updated);
			}
			entries_.push_back(std::move(entry));
		}

		Finish(!result.next_page_token.empty());
		if (!result.next_page_token.empty()) {
			page_token_ = result.next_page_token;
		}
		return;
	}

private:
	void Finish(bool more)
	{
		if (more && pages_ < 10000) {
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

	void log_error()
	{
		std::string message;
		if (rr_ && gcs::parse_error(std::string(rr_->response_.body_.to_view()), message)) {
			log(logmsg::error, _("Server error: %s"), to_wstring(message));
		}
		else {
			log(logmsg::error, _("Server returned HTTP status %d"), static_cast<int>(rr_ ? rr_->response_.code_ : 0));
		}
	}

	std::shared_ptr<GcsRequestResponse> rr_;
	CServerPath path_;
	std::string bucket_;
	std::string prefix_;
	std::string page_token_;
	std::vector<CDirentry> entries_;
	int pages_{};
};

// ---------------------------------------------------------------------------
// CGcsFileTransferOpData
// ---------------------------------------------------------------------------

class CGcsFileTransferOpData final : public CFileTransferOpData, public CGcsOpData, public CGcsRequestHandler
{
public:
	CGcsFileTransferOpData(CGcsControlSocket & controlSocket, CFileTransferCommand const& cmd)
		: CFileTransferOpData(L"CGcsFileTransferOpData", cmd)
		, CGcsOpData(controlSocket)
	{}

	virtual int Send() override
	{
		if (opState == 0) {
			auto split = CGcsControlSocket::SplitPath(remotePath_);
			bucket_ = split.first;
			object_ = gcs::join_name(split.second, fz::to_utf8(remoteFile_));
			if (bucket_.empty() || object_.empty()) {
				log(logmsg::error, _("Invalid path"));
				return FZ_REPLY_ERROR;
			}

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
			opState = 1;
		}

		rr_ = std::make_shared<GcsRequestResponse>();

		if (download()) {
			rr_->set_on_header([this](std::shared_ptr<GcsRequestResponse> const& srr) { return OnHeader(srr); });

			std::map<std::string, std::string> headers;
			if (resume_ && localFileSize_ != 0 && localFileSize_ != fz::aio_base::nosize) {
				headers["Range"] = "bytes=" + std::to_string(localFileSize_) + "-";
			}

			if (!controlSocket_.Submit(rr_, gcs::object_url(bucket_, object_), "GET", headers, this)) {
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

		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/octet-stream";
		if (!controlSocket_.Submit(rr_, gcs::upload_url(bucket_, object_), "POST", headers, this)) {
			return FZ_REPLY_ERROR;
		}
		rr_->request_.update_content_length_from_body();
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnGcsResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			std::string message;
			if (rr_ && gcs::parse_error(std::string(rr_->response_.body_.to_view()), message)) {
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
	fz::http::continuation OnHeader(std::shared_ptr<GcsRequestResponse> const& srr)
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

	std::shared_ptr<GcsRequestResponse> rr_;
	std::string bucket_;
	std::string object_;
};

// ---------------------------------------------------------------------------
// CGcsDeleteOpData
// ---------------------------------------------------------------------------

class CGcsDeleteOpData final : public COpData, public CGcsOpData, public CGcsRequestHandler
{
public:
	CGcsDeleteOpData(CGcsControlSocket & controlSocket, CServerPath const& path, std::vector<std::wstring> files)
		: COpData(Command::del, L"CGcsDeleteOpData")
		, CGcsOpData(controlSocket)
		, path_(path)
		, files_(std::move(files))
	{
		auto split = CGcsControlSocket::SplitPath(path_);
		bucket_ = split.first;
		prefix_ = split.second;
	}

	virtual int Send() override
	{
		if (files_.empty()) {
			return FZ_REPLY_OK;
		}

		rr_ = std::make_shared<GcsRequestResponse>();
		current_ = files_.back();
		std::string const object = gcs::join_name(prefix_, fz::to_utf8(current_));

		if (!controlSocket_.Submit(rr_, gcs::object_json_url(bucket_, object), "DELETE", {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnGcsResponse(bool success) override
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
	std::string bucket_;
	std::string prefix_;
	std::shared_ptr<GcsRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CGcsMkdirOpData
// ---------------------------------------------------------------------------

class CGcsMkdirOpData final : public COpData, public CGcsOpData, public CGcsRequestHandler
{
public:
	CGcsMkdirOpData(CGcsControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::mkdir, L"CGcsMkdirOpData")
		, CGcsOpData(controlSocket)
		, path_(path)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<GcsRequestResponse>();

		auto split = CGcsControlSocket::SplitPath(path_);
		bucket_ = split.first;
		std::string name = split.second;
		if (name.empty()) {
			log(logmsg::error, _("Invalid path"));
			return FZ_REPLY_ERROR;
		}
		if (name.back() != '/') {
			name += '/';
		}

		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/octet-stream";
		headers["Content-Length"] = "0";

		if (!controlSocket_.Submit(rr_, gcs::upload_url(bucket_, name), "POST", headers, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnGcsResponse(bool success) override
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
	std::string bucket_;
	std::shared_ptr<GcsRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CGcsRenameOpData (rewrite + delete)
// ---------------------------------------------------------------------------

class CGcsRenameOpData final : public COpData, public CGcsOpData, public CGcsRequestHandler
{
public:
	CGcsRenameOpData(CGcsControlSocket & controlSocket, CRenameCommand const& command)
		: COpData(Command::rename, L"CGcsRenameOpData")
		, CGcsOpData(controlSocket)
		, command_(command)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<GcsRequestResponse>();

		auto from = CGcsControlSocket::SplitPath(command_.GetFromPath());
		auto to = CGcsControlSocket::SplitPath(command_.GetToPath());
		bucket_ = from.first;
		dest_bucket_ = to.first;
		source_ = gcs::join_name(from.second, fz::to_utf8(command_.GetFromFile()));
		target_ = gcs::join_name(to.second, fz::to_utf8(command_.GetToFile()));

		std::string url;
		std::string verb;
		if (step_ == 0) {
			url = "https://storage.googleapis.com/storage/v1/b/" + gcs::encode_object_name(bucket_) + "/o/" +
				gcs::encode_object_name(source_) + "/rewriteTo/b/" + gcs::encode_object_name(dest_bucket_) +
				"/o/" + gcs::encode_object_name(target_);
			verb = "POST";
		}
		else {
			url = gcs::object_json_url(bucket_, source_);
			verb = "DELETE";
		}

		if (!controlSocket_.Submit(rr_, url, verb, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnGcsResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Renaming file failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (step_ == 0) {
			step_ = 1;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}

		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	CRenameCommand command_;
	std::string bucket_;
	std::string dest_bucket_;
	std::string source_;
	std::string target_;
	int step_{};
	std::shared_ptr<GcsRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CGcsControlSocket
// ---------------------------------------------------------------------------

CGcsControlSocket::CGcsControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	client_.emplace(*this);
}

CGcsControlSocket::~CGcsControlSocket()
{
	client_.reset();
}

void CGcsControlSocket::ResetSocket()
{
	tls_layer_.reset();
	CRealControlSocket::ResetSocket();
}

void CGcsControlSocket::Connect(CServer const& server, Credentials const& credentials)
{
	currentServer_ = server;
	credentials_ = credentials;

	token_ = fz::to_utf8(credentials.GetPass());
	project_ = fz::to_utf8(server.GetExtraParameter("project"));

	if (project_.empty()) {
		project_ = fz::to_utf8(server.GetExtraParameter("project_id"));
	}

	// The API host is fixed by the vendor; "api_endpoint" (optionally with its own
	// scheme and port) and "tls" allow reaching a private gateway or an emulator.
	auto const api = fz::to_utf8(server.GetExtraParameter("api_endpoint"));
	has_endpoint_override_ = !api.empty();
	endpoint_ = cloud::resolve(has_endpoint_override_ ? api : std::string("storage.googleapis.com"),
		fz::to_utf8(server.GetExtraParameter("tls")), 443);

	Push(std::make_unique<CGcsConnectOpData>(*this));
}

std::pair<std::string, std::string> CGcsControlSocket::SplitPath(CServerPath const& path)
{
	std::string const full = fz::to_utf8(path.GetPath());
	if (full.empty() || full == "/") {
		return {"", ""};
	}

	std::string rest = full;
	if (rest.front() == '/') {
		rest.erase(0, 1);
	}

	auto const pos = rest.find('/');
	if (pos == std::string::npos) {
		return {rest, ""};
	}

	std::string prefix = rest.substr(pos + 1);
	if (!prefix.empty() && prefix.back() != '/') {
		prefix += '/';
	}
	return {rest.substr(0, pos), prefix};
}

bool CGcsControlSocket::Submit(std::shared_ptr<GcsRequestResponse> const& rr, std::string const& url,
	std::string const& verb, std::map<std::string, std::string> extra_headers, CGcsRequestHandler * handler)
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

	if (verb == "POST" && !req.body_ && !extra_headers.count("Content-Length")) {
		// Requests without a body (e.g. rewrite) still need an explicit length
		req.headers_["Content-Length"] = "0";
	}

	pending_handler_ = handler;
	if (!client_->add_request(rr)) {
		pending_handler_ = nullptr;
		return false;
	}
	return true;
}

void CGcsControlSocket::OnRequestDone(uint64_t, bool success)
{
	auto * handler = pending_handler_;
	pending_handler_ = nullptr;
	if (handler) {
		handler->OnGcsResponse(success);
	}
}

void CGcsControlSocket::OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info)
{
	if (!tls_layer_ || source != tls_layer_.get()) {
		return;
	}
	SendAsyncRequest(std::make_unique<CCertificateNotification>(std::move(info)));
}

void CGcsControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::certificate_verification_event, fz::http::client::done_event>(ev, this,
			&CGcsControlSocket::OnVerifyCert, &CGcsControlSocket::OnRequestDone))
	{
		return;
	}
	CRealControlSocket::operator()(ev);
}

void CGcsControlSocket::List(CServerPath const& path, int)
{
	Push(std::make_unique<CGcsListOpData>(*this, path));
}

void CGcsControlSocket::FileTransfer(CFileTransferCommand const& cmd)
{
	Push(std::make_unique<CGcsFileTransferOpData>(*this, cmd));
}

void CGcsControlSocket::Delete(CServerPath const& path, std::vector<std::wstring>&& files)
{
	Push(std::make_unique<CGcsDeleteOpData>(*this, path, std::move(files)));
}

void CGcsControlSocket::RemoveDir(CServerPath const&, std::wstring const&)
{
	log(logmsg::error, _("Removing directories is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

void CGcsControlSocket::Mkdir(CServerPath const& path, transfer_flags const&)
{
	Push(std::make_unique<CGcsMkdirOpData>(*this, path));
}

void CGcsControlSocket::Rename(CRenameCommand const& command)
{
	Push(std::make_unique<CGcsRenameOpData>(*this, command));
}

void CGcsControlSocket::Chmod(CChmodCommand const&)
{
	log(logmsg::status, _("Setting permissions is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

int CGcsControlSocket::DoClose(int nErrorCode)
{
	pending_handler_ = nullptr;
	if (client_) {
		client_->stop(true);
	}
	tls_layer_.reset();

	return CRealControlSocket::DoClose(nErrorCode);
}

bool CGcsControlSocket::SetAsyncRequestReply(CAsyncRequestNotification * pNotification)
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
