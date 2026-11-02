#include "../filezilla.h"

#include "azurecontrolsocket.h"

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

std::string const api_version = "2021-08-06";

std::wstring to_wstring(std::string const& in)
{
	return fz::to_wstring_from_utf8(in);
}

std::string encode_value(std::string const& in)
{
	static char const hex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(in.size());
	for (unsigned char const c : in) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '.' || c == '_' || c == '~')
		{
			out.push_back(static_cast<char>(c));
		}
		else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0x0f]);
		}
	}
	return out;
}

std::string encode_query(std::vector<std::pair<std::string, std::string>> const& query)
{
	std::string ret;
	for (auto const& q : query) {
		if (!ret.empty()) {
			ret += '&';
		}
		ret += encode_value(q.first);
		ret += '=';
		ret += encode_value(q.second);
	}
	return ret;
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
// azure_http_client
// ---------------------------------------------------------------------------

azure_http_client::azure_http_client(CAzureBlobControlSocket & owner)
	: fz::http::client::client(owner, *owner.buffer_pool_, owner.logger(), "FileZilla/"s + ENGINE_VERSION)
	, owner_(owner)
{
}

azure_http_client::~azure_http_client()
{
	destroy();
}

fz::socket_interface* azure_http_client::create_socket(fz::native_string const& host, unsigned short, bool tls)
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

void azure_http_client::destroy_socket()
{
	owner_.ResetSocket();
}

void azure_http_client::on_alive()
{
	owner_.SetAlive();
}

// ---------------------------------------------------------------------------
// CAzureConnectOpData
// ---------------------------------------------------------------------------

class CAzureConnectOpData final : public COpData, public CAzureOpData
{
public:
	explicit CAzureConnectOpData(CAzureBlobControlSocket & controlSocket)
		: COpData(Command::connect, L"CAzureConnectOpData")
		, CAzureOpData(controlSocket)
	{}

	virtual int Send() override
	{
		if (controlSocket_.account_.empty() || controlSocket_.cred_.key_base64.empty()) {
			log(logmsg::error, _("An account name and an account key are required."));
			return FZ_REPLY_CRITICALERROR;
		}
		return FZ_REPLY_OK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }
};

// ---------------------------------------------------------------------------
// CAzureListOpData
// ---------------------------------------------------------------------------

class CAzureListOpData final : public COpData, public CAzureOpData, public CAzureRequestHandler
{
public:
	CAzureListOpData(CAzureBlobControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::list, L"CAzureListOpData")
		, CAzureOpData(controlSocket)
		, path_(path)
	{
		auto split = CAzureBlobControlSocket::SplitPath(path_);
		container_ = split.first;
		prefix_ = split.second;
	}

	virtual int Send() override
	{
		rr_ = std::make_shared<AzureRequestResponse>();

		std::vector<std::pair<std::string, std::string>> query;
		std::string raw_path;

		if (container_.empty()) {
			raw_path = "/";
			query.emplace_back("comp", "list");
			if (!marker_.empty()) {
				query.emplace_back("marker", marker_);
			}
		}
		else {
			raw_path = "/" + container_;
			query.emplace_back("restype", "container");
			query.emplace_back("comp", "list");
			query.emplace_back("delimiter", "/");
			if (!prefix_.empty()) {
				query.emplace_back("prefix", prefix_);
			}
			if (!marker_.empty()) {
				query.emplace_back("marker", marker_);
			}
		}

		if (!controlSocket_.Submit(rr_, "GET", raw_path, query, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnAzureResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			std::string code, message;
			if (rr_ && azure::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
				log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
			}
			else {
				log(logmsg::error, _("Server returned HTTP status %d"), static_cast<int>(rr_ ? rr_->response_.code_ : 0));
			}
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());

		if (container_.empty()) {
			std::vector<azure::container_entry> containers;
			std::string next_marker;
			if (!azure::parse_list_containers(body, containers, next_marker)) {
				log(logmsg::error, _("Could not parse the server's response."));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			for (auto const& c : containers) {
				CDirentry entry;
				entry.name = to_wstring(c.name);
				entry.size = -1;
				entry.flags = CDirentry::flag_dir;
				entries_.push_back(std::move(entry));
			}
			if (!next_marker.empty() && pages_ < 10000) {
				++pages_;
				marker_ = next_marker;
				rr_.reset();
				controlSocket_.SendNextCommand();
				return;
			}
			Finish();
			return;
		}

		azure::list_blobs_result result;
		if (!azure::parse_list_blobs(body, result)) {
			log(logmsg::error, _("Could not parse the server's response."));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		for (auto const& b : result.blobs) {
			bool is_dir = b.is_prefix;
			std::wstring const name = name_from_key(b.name, prefix_, is_dir);
			if (name.empty()) {
				continue;
			}
			CDirentry entry;
			entry.name = name;
			entry.size = is_dir ? -1 : b.size;
			entry.flags = is_dir ? CDirentry::flag_dir : 0;
			if (!b.last_modified.empty()) {
				entry.time.set_rfc822(b.last_modified);
			}
			entries_.push_back(std::move(entry));
		}

		if (!result.next_marker.empty() && pages_ < 10000) {
			++pages_;
			marker_ = result.next_marker;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}

		Finish();
	}

private:
	void Finish()
	{
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

	std::shared_ptr<AzureRequestResponse> rr_;
	CServerPath path_;
	std::string container_;
	std::string prefix_;
	std::string marker_;
	std::vector<CDirentry> entries_;
	int pages_{};
};

// ---------------------------------------------------------------------------
// CAzureFileTransferOpData
// ---------------------------------------------------------------------------

class CAzureFileTransferOpData final : public CFileTransferOpData, public CAzureOpData, public CAzureRequestHandler
{
public:
	CAzureFileTransferOpData(CAzureBlobControlSocket & controlSocket, CFileTransferCommand const& cmd)
		: CFileTransferOpData(L"CAzureFileTransferOpData", cmd)
		, CAzureOpData(controlSocket)
	{}

	virtual int Send() override
	{
		if (opState == 0) {
			auto split = CAzureBlobControlSocket::SplitPath(remotePath_);
			container_ = split.first;
			blob_ = azure::join_name(split.second, fz::to_utf8(remoteFile_));
			if (container_.empty() || blob_.empty()) {
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

		rr_ = std::make_shared<AzureRequestResponse>();
		std::string const raw_path = "/" + container_ + "/" + blob_;

		if (download()) {
			rr_->set_on_header([this](std::shared_ptr<AzureRequestResponse> const& srr) { return OnHeader(srr); });

			std::map<std::string, std::string> headers;
			if (resume_ && localFileSize_ != 0 && localFileSize_ != fz::aio_base::nosize) {
				headers["Range"] = "bytes=" + std::to_string(localFileSize_) + "-";
			}

			if (!controlSocket_.Submit(rr_, "GET", raw_path, {}, headers, this)) {
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

		uint64_t size = reader_factory_->size();
		std::map<std::string, std::string> headers;
		headers["x-ms-blob-type"] = "BlockBlob";
		headers["Content-Type"] = "application/octet-stream";
		if (size != fz::aio_base::nosize) {
			headers["Content-Length"] = std::to_string(size);
		}

		if (!controlSocket_.Submit(rr_, "PUT", raw_path, {}, headers, this)) {
			return FZ_REPLY_ERROR;
		}
		if (size == fz::aio_base::nosize) {
			rr_->request_.update_content_length_from_body();
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnAzureResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			std::string code, message;
			if (rr_ && azure::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
				log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
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
	fz::http::continuation OnHeader(std::shared_ptr<AzureRequestResponse> const& srr)
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

	std::shared_ptr<AzureRequestResponse> rr_;
	std::string container_;
	std::string blob_;
};

// ---------------------------------------------------------------------------
// CAzureDeleteOpData
// ---------------------------------------------------------------------------

class CAzureDeleteOpData final : public COpData, public CAzureOpData, public CAzureRequestHandler
{
public:
	CAzureDeleteOpData(CAzureBlobControlSocket & controlSocket, CServerPath const& path, std::vector<std::wstring> files)
		: COpData(Command::del, L"CAzureDeleteOpData")
		, CAzureOpData(controlSocket)
		, path_(path)
		, files_(std::move(files))
	{
		auto split = CAzureBlobControlSocket::SplitPath(path_);
		container_ = split.first;
		prefix_ = split.second;
	}

	virtual int Send() override
	{
		if (files_.empty()) {
			return FZ_REPLY_OK;
		}

		rr_ = std::make_shared<AzureRequestResponse>();
		current_ = files_.back();
		std::string const raw_path = "/" + container_ + "/" + azure::join_name(prefix_, fz::to_utf8(current_));

		if (!controlSocket_.Submit(rr_, "DELETE", raw_path, {}, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnAzureResponse(bool success) override
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
	std::string container_;
	std::string prefix_;
	std::shared_ptr<AzureRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CAzureMkdirOpData
// ---------------------------------------------------------------------------

class CAzureMkdirOpData final : public COpData, public CAzureOpData, public CAzureRequestHandler
{
public:
	CAzureMkdirOpData(CAzureBlobControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::mkdir, L"CAzureMkdirOpData")
		, CAzureOpData(controlSocket)
		, path_(path)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<AzureRequestResponse>();

		auto split = CAzureBlobControlSocket::SplitPath(path_);
		container_ = split.first;
		std::string name = split.second;
		if (name.empty()) {
			log(logmsg::error, _("Invalid path"));
			return FZ_REPLY_ERROR;
		}
		if (name.back() != '/') {
			name += '/';
		}

		std::map<std::string, std::string> headers;
		headers["x-ms-blob-type"] = "BlockBlob";
		headers["Content-Length"] = "0";
		headers["Content-Type"] = "application/octet-stream";

		if (!controlSocket_.Submit(rr_, "PUT", "/" + container_ + "/" + name, {}, headers, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnAzureResponse(bool success) override
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
	std::string container_;
	std::shared_ptr<AzureRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CAzureRenameOpData (copy + delete)
// ---------------------------------------------------------------------------

class CAzureRenameOpData final : public COpData, public CAzureOpData, public CAzureRequestHandler
{
public:
	CAzureRenameOpData(CAzureBlobControlSocket & controlSocket, CRenameCommand const& command)
		: COpData(Command::rename, L"CAzureRenameOpData")
		, CAzureOpData(controlSocket)
		, command_(command)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<AzureRequestResponse>();

		auto from = CAzureBlobControlSocket::SplitPath(command_.GetFromPath());
		auto to = CAzureBlobControlSocket::SplitPath(command_.GetToPath());
		container_ = from.first;
		source_ = "/" + from.first + "/" + azure::join_name(from.second, fz::to_utf8(command_.GetFromFile()));
		target_ = "/" + to.first + "/" + azure::join_name(to.second, fz::to_utf8(command_.GetToFile()));

		if (step_ == 0) {
			std::map<std::string, std::string> headers;
			// The copy source must be a fully qualified URL
			headers["x-ms-copy-source"] = "https://" + controlSocket_.host_ + azure::encode_blob_name(source_);
			if (!controlSocket_.Submit(rr_, "PUT", target_, {}, headers, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		if (!controlSocket_.Submit(rr_, "DELETE", source_, {}, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnAzureResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Renaming file failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (step_ == 0) {
			if (auto const status = rr_->response_.get_header("x-ms-copy-status"); status == "pending") {
				log(logmsg::debug_warning, _("The server is still copying the blob, the source may not be removed yet."));
			}
			step_ = 1;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}

		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	CRenameCommand command_;
	std::string container_;
	std::string source_;
	std::string target_;
	int step_{};
	std::shared_ptr<AzureRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CAzureBlobControlSocket
// ---------------------------------------------------------------------------

CAzureBlobControlSocket::CAzureBlobControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	client_.emplace(*this);
}

CAzureBlobControlSocket::~CAzureBlobControlSocket()
{
	client_.reset();
}

void CAzureBlobControlSocket::ResetSocket()
{
	tls_layer_.reset();
	CRealControlSocket::ResetSocket();
}

void CAzureBlobControlSocket::Connect(CServer const& server, Credentials const& credentials)
{
	currentServer_ = server;
	credentials_ = credentials;

	account_ = fz::to_utf8(server.GetUser());
	cred_.account = account_;
	cred_.key_base64 = fz::to_utf8(credentials.GetPass());

	host_ = fz::to_utf8(server.GetHost());
	if (host_.empty() && !account_.empty()) {
		host_ = account_ + ".blob.core.windows.net";
	}

	// Azure requires the account name to match the one in the Authorization header
	if (account_.empty()) {
		auto const pos = host_.find('.');
		if (pos != std::string::npos) {
			account_ = host_.substr(0, pos);
			cred_.account = account_;
		}
	}

	Push(std::make_unique<CAzureConnectOpData>(*this));
}

std::pair<std::string, std::string> CAzureBlobControlSocket::SplitPath(CServerPath const& path)
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

bool CAzureBlobControlSocket::Submit(std::shared_ptr<AzureRequestResponse> const& rr, std::string const& verb,
	std::string const& path, std::vector<std::pair<std::string, std::string>> query,
	std::map<std::string, std::string> extra_headers, CAzureRequestHandler * handler)
{
	if (!client_ || !rr) {
		return false;
	}

	std::map<std::string, std::string> headers = extra_headers;
	headers["x-ms-date"] = azure::rfc1123_now();
	headers["x-ms-version"] = api_version;

	auto const authorization = azure::sign(cred_, verb, headers, path, query);

	auto & req = rr->request_;
	req.verb_ = verb;
	req.uri_.scheme_ = "https";
	req.uri_.host_ = host_;
	req.uri_.path_ = path;
	req.uri_.query_ = encode_query(query);

	for (auto const& h : headers) {
		req.headers_[h.first] = h.second;
	}
	req.headers_["Authorization"] = authorization;

	pending_handler_ = handler;
	if (!client_->add_request(rr)) {
		pending_handler_ = nullptr;
		return false;
	}
	return true;
}

void CAzureBlobControlSocket::OnRequestDone(uint64_t, bool success)
{
	auto * handler = pending_handler_;
	pending_handler_ = nullptr;
	if (handler) {
		handler->OnAzureResponse(success);
	}
}

void CAzureBlobControlSocket::OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info)
{
	if (!tls_layer_ || source != tls_layer_.get()) {
		return;
	}
	SendAsyncRequest(std::make_unique<CCertificateNotification>(std::move(info)));
}

void CAzureBlobControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::certificate_verification_event, fz::http::client::done_event>(ev, this,
			&CAzureBlobControlSocket::OnVerifyCert, &CAzureBlobControlSocket::OnRequestDone))
	{
		return;
	}
	CRealControlSocket::operator()(ev);
}

void CAzureBlobControlSocket::List(CServerPath const& path, int)
{
	Push(std::make_unique<CAzureListOpData>(*this, path));
}

void CAzureBlobControlSocket::FileTransfer(CFileTransferCommand const& cmd)
{
	Push(std::make_unique<CAzureFileTransferOpData>(*this, cmd));
}

void CAzureBlobControlSocket::Delete(CServerPath const& path, std::vector<std::wstring>&& files)
{
	Push(std::make_unique<CAzureDeleteOpData>(*this, path, std::move(files)));
}

void CAzureBlobControlSocket::RemoveDir(CServerPath const&, std::wstring const&)
{
	log(logmsg::error, _("Removing directories is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

void CAzureBlobControlSocket::Mkdir(CServerPath const& path, transfer_flags const&)
{
	Push(std::make_unique<CAzureMkdirOpData>(*this, path));
}

void CAzureBlobControlSocket::Rename(CRenameCommand const& command)
{
	Push(std::make_unique<CAzureRenameOpData>(*this, command));
}

void CAzureBlobControlSocket::Chmod(CChmodCommand const&)
{
	log(logmsg::status, _("Setting permissions is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

int CAzureBlobControlSocket::DoClose(int nErrorCode)
{
	pending_handler_ = nullptr;
	if (client_) {
		client_->stop(true);
	}
	tls_layer_.reset();

	return CRealControlSocket::DoClose(nErrorCode);
}

bool CAzureBlobControlSocket::SetAsyncRequestReply(CAsyncRequestNotification * pNotification)
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
