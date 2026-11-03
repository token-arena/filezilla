#include "../filezilla.h"

#include "coscontrolsocket.h"

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

std::string encode_query_component(std::string const& value)
{
	static char const hex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size());
	for (unsigned char const c : value) {
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

}

// ---------------------------------------------------------------------------
// cos_http_client
// ---------------------------------------------------------------------------

cos_http_client::cos_http_client(CCosControlSocket & owner)
	: fz::http::client::client(owner, *owner.buffer_pool_, owner.logger(), "FileZilla/"s + ENGINE_VERSION)
	, owner_(owner)
{
}

cos_http_client::~cos_http_client()
{
	destroy();
}

fz::socket_interface* cos_http_client::create_socket(fz::native_string const& host, unsigned short, bool tls)
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

void cos_http_client::destroy_socket()
{
	owner_.ResetSocket();
}

void cos_http_client::on_alive()
{
	owner_.SetAlive();
}

// ---------------------------------------------------------------------------
// CCosConnectOpData
// ---------------------------------------------------------------------------

class CCosConnectOpData final : public COpData, public CCosOpData
{
public:
	explicit CCosConnectOpData(CCosControlSocket & controlSocket)
		: COpData(Command::connect, L"CCosConnectOpData")
		, CCosOpData(controlSocket)
	{}

	virtual int Send() override
	{
		if (controlSocket_.cred_.secret_id.empty() || controlSocket_.cred_.secret_key.empty()) {
			log(logmsg::error, _("A secret id and a secret key are required."));
			return FZ_REPLY_CRITICALERROR;
		}
		if (controlSocket_.host_.empty()) {
			log(logmsg::error, _("The bucket domain (for example bucket-appid.cos.ap-beijing.myqcloud.com) is required as host."));
			return FZ_REPLY_CRITICALERROR;
		}
		return FZ_REPLY_OK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }
};

// ---------------------------------------------------------------------------
// CCosListOpData
// ---------------------------------------------------------------------------

class CCosListOpData final : public COpData, public CCosOpData, public CCosRequestHandler
{
public:
	CCosListOpData(CCosControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::list, L"CCosListOpData")
		, CCosOpData(controlSocket)
		, path_(path)
	{
		prefix_ = CCosControlSocket::KeyPrefix(path_);
	}

	virtual int Send() override
	{
		rr_ = std::make_shared<CosRequestResponse>();

		std::vector<std::pair<std::string, std::string>> query;
		if (!prefix_.empty()) {
			query.emplace_back("prefix", prefix_);
		}
		query.emplace_back("delimiter", "/");
		query.emplace_back("max-keys", "1000");
		if (!marker_.empty()) {
			query.emplace_back("marker", marker_);
		}

		if (!controlSocket_.Submit(rr_, "GET", "", query, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnCosResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log_error();
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		tcos::list_result result;
		if (!tcos::parse_list_objects(std::string(rr_->response_.body_.to_view()), result)) {
			log(logmsg::error, _("Could not parse the server's response."));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		for (auto const& o : result.entries) {
			bool is_dir = o.is_prefix;
			std::wstring const name = name_from_key(o.key, prefix_, is_dir);
			if (name.empty()) {
				continue;
			}
			CDirentry entry;
			entry.name = name;
			entry.size = is_dir ? -1 : o.size;
			entry.flags = is_dir ? CDirentry::flag_dir : 0;
			if (!o.last_modified.empty()) {
				entry.time.set_rfc3339(o.last_modified);
			}
			entries_.push_back(std::move(entry));
		}

		if (result.truncated && !result.next_marker.empty() && pages_ < 10000) {
			++pages_;
			marker_ = result.next_marker;
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
		if (rr_ && tcos::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
			log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
		}
		else {
			log(logmsg::error, _("Server returned HTTP status %d"), static_cast<int>(rr_ ? rr_->response_.code_ : 0));
		}
	}

	std::shared_ptr<CosRequestResponse> rr_;
	CServerPath path_;
	std::string prefix_;
	std::string marker_;
	std::vector<CDirentry> entries_;
	int pages_{};
};

// ---------------------------------------------------------------------------
// CCosFileTransferOpData
// ---------------------------------------------------------------------------

class CCosFileTransferOpData final : public CFileTransferOpData, public CCosOpData, public CCosRequestHandler
{
public:
	CCosFileTransferOpData(CCosControlSocket & controlSocket, CFileTransferCommand const& cmd)
		: CFileTransferOpData(L"CCosFileTransferOpData", cmd)
		, CCosOpData(controlSocket)
	{}

	virtual int Send() override
	{
		for (;;) {
			switch (stage_) {
			case stage_start:
			{
				key_ = tcos::join_name(CCosControlSocket::KeyPrefix(remotePath_), fz::to_utf8(remoteFile_));
				if (key_.empty()) {
					log(logmsg::error, _("Invalid path"));
					return FZ_REPLY_ERROR;
				}

				if (download()) {
					int res = controlSocket_.CheckOverwriteFile();
					if (res != FZ_REPLY_OK) {
						return res;
					}
					stage_ = stage_download;
					continue;
				}

				if (!reader_factory_) {
					log(logmsg::error, _("No local file to send."));
					return FZ_REPLY_ERROR;
				}

				upload_size_ = reader_factory_->size();
				multipart_ = reader_factory_->seekable() &&
					upload_size_ != fz::aio_base::nosize &&
					upload_size_ > part_size_;
				if (multipart_) {
					part_count_ = static_cast<int>((upload_size_ + part_size_ - 1) / part_size_);
				}

				stage_ = multipart_ ? stage_initiate : stage_single_put;
				continue;
			}
			case stage_download:
			{
				rr_ = std::make_shared<CosRequestResponse>();
				std::map<std::string, std::string> headers;
				if (resume_ && localFileSize_ != 0 && localFileSize_ != fz::aio_base::nosize) {
					headers["Range"] = "bytes=" + std::to_string(localFileSize_) + "-";
				}
				rr_->set_on_header([this](std::shared_ptr<CosRequestResponse> const& srr) { return OnHeader(srr); });
				if (!controlSocket_.Submit(rr_, "GET", key_, {}, headers, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_single_put:
			{
				rr_ = std::make_shared<CosRequestResponse>();
				rr_->request_.body_ = reader_factory_->open(*controlSocket_.buffer_pool_, 0, fz::aio_base::nosize,
					controlSocket_.max_buffer_count());
				if (!rr_->request_.body_) {
					log(logmsg::error, _("Could not open the local file."));
					return FZ_REPLY_ERROR;
				}
				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/octet-stream";
				if (!controlSocket_.Submit(rr_, "PUT", key_, {}, headers, this)) {
					return FZ_REPLY_ERROR;
				}
				rr_->request_.update_content_length_from_body();
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_initiate:
			{
				rr_ = std::make_shared<CosRequestResponse>();
				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/octet-stream";
				if (!controlSocket_.Submit(rr_, "POST", key_, {{"uploads", ""}}, headers, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_part:
			{
				if (current_part_ > part_count_) {
					stage_ = stage_complete;
					continue;
				}

				auto const offset = static_cast<uint64_t>(current_part_ - 1) * part_size_;
				auto const this_size = std::min<uint64_t>(part_size_, upload_size_ - offset);

				rr_ = std::make_shared<CosRequestResponse>();
				rr_->request_.body_ = reader_factory_->open(*controlSocket_.buffer_pool_, offset, this_size,
					controlSocket_.max_buffer_count());
				if (!rr_->request_.body_) {
					log(logmsg::error, _("Could not open the local file."));
					return FZ_REPLY_ERROR;
				}

				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/octet-stream";
				std::vector<std::pair<std::string, std::string>> query = {
					{"partNumber", std::to_string(current_part_)}, {"uploadId", upload_id_}};

				if (!controlSocket_.Submit(rr_, "PUT", key_, query, headers, this)) {
					return FZ_REPLY_ERROR;
				}
				rr_->request_.update_content_length_from_body();
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_complete:
			{
				rr_ = std::make_shared<CosRequestResponse>();
				std::string const body = tcos::build_complete_multipart(parts_);
				rr_->request_.body_ = fz::string_reader_factory(L"complete", body).open(*controlSocket_.buffer_pool_);
				if (!rr_->request_.body_) {
					return FZ_REPLY_ERROR;
				}
				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/xml";
				if (!controlSocket_.Submit(rr_, "POST", key_, {{"uploadId", upload_id_}}, headers, this)) {
					return FZ_REPLY_ERROR;
				}
				rr_->request_.update_content_length_from_body();
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_abort:
			{
				rr_ = std::make_shared<CosRequestResponse>();
				if (!controlSocket_.Submit(rr_, "DELETE", key_, {{"uploadId", upload_id_}}, {}, this)) {
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

	virtual void OnCosResponse(bool success) override
	{
		if (stage_ == stage_abort) {
			log(logmsg::debug_warning, L"Multipart upload aborted");
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (!rr_ || !success || !rr_->response_.success()) {
			std::string code, message;
			if (rr_ && tcos::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
				log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
			}
			else {
				log(logmsg::error, _("File transfer failed"));
			}
			if (multipart_ && !upload_id_.empty() && stage_ != stage_initiate) {
				stage_ = stage_abort;
				rr_.reset();
				controlSocket_.SendNextCommand();
				return;
			}
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		switch (stage_) {
		case stage_initiate:
			if (!tcos::parse_initiate_multipart(std::string(rr_->response_.body_.to_view()), upload_id_)) {
				log(logmsg::error, _("Could not parse the server's response."));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			current_part_ = 1;
			stage_ = stage_part;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		case stage_part:
		{
			auto etag = rr_->response_.get_header("etag");
			if (etag.empty()) {
				log(logmsg::error, _("The server did not return an entity tag for part %d"), current_part_);
				stage_ = stage_abort;
				rr_.reset();
				controlSocket_.SendNextCommand();
				return;
			}
			parts_.push_back({current_part_, etag});
			++current_part_;
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
	enum stages : int {
		stage_start = 0,
		stage_download,
		stage_single_put,
		stage_initiate,
		stage_part,
		stage_complete,
		stage_abort
	};

	fz::http::continuation OnHeader(std::shared_ptr<CosRequestResponse> const& srr)
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

	std::shared_ptr<CosRequestResponse> rr_;
	std::string key_;
	std::string upload_id_;
	std::vector<tcos::part_entry> parts_;
	uint64_t part_size_{8 * 1024 * 1024};
	uint64_t upload_size_{};
	int part_count_{};
	int current_part_{};
	int stage_{stage_start};
	bool multipart_{};
};

// ---------------------------------------------------------------------------
// CCosDeleteOpData
// ---------------------------------------------------------------------------

class CCosDeleteOpData final : public COpData, public CCosOpData, public CCosRequestHandler
{
public:
	CCosDeleteOpData(CCosControlSocket & controlSocket, CServerPath const& path, std::vector<std::wstring> files)
		: COpData(Command::del, L"CCosDeleteOpData")
		, CCosOpData(controlSocket)
		, path_(path)
		, files_(std::move(files))
	{
		prefix_ = CCosControlSocket::KeyPrefix(path_);
	}

	virtual int Send() override
	{
		if (files_.empty()) {
			return FZ_REPLY_OK;
		}

		rr_ = std::make_shared<CosRequestResponse>();
		current_ = files_.back();
		std::string const key = tcos::join_name(prefix_, fz::to_utf8(current_));

		if (!controlSocket_.Submit(rr_, "DELETE", key, {}, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnCosResponse(bool success) override
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
	std::string prefix_;
	std::shared_ptr<CosRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CCosMkdirOpData
// ---------------------------------------------------------------------------

class CCosMkdirOpData final : public COpData, public CCosOpData, public CCosRequestHandler
{
public:
	CCosMkdirOpData(CCosControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::mkdir, L"CCosMkdirOpData")
		, CCosOpData(controlSocket)
		, path_(path)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<CosRequestResponse>();

		std::string key = CCosControlSocket::KeyPrefix(path_);
		if (key.empty()) {
			log(logmsg::error, _("Invalid path"));
			return FZ_REPLY_ERROR;
		}

		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/octet-stream";
		headers["Content-Length"] = "0";

		if (!controlSocket_.Submit(rr_, "PUT", key, {}, headers, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnCosResponse(bool success) override
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
	std::shared_ptr<CosRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CCosRenameOpData (server side copy + delete)
// ---------------------------------------------------------------------------

class CCosRenameOpData final : public COpData, public CCosOpData, public CCosRequestHandler
{
public:
	CCosRenameOpData(CCosControlSocket & controlSocket, CRenameCommand const& command)
		: COpData(Command::rename, L"CCosRenameOpData")
		, CCosOpData(controlSocket)
		, command_(command)
	{
		source_key_ = tcos::join_name(CCosControlSocket::KeyPrefix(command_.GetFromPath()),
			fz::to_utf8(command_.GetFromFile()));
		dest_key_ = tcos::join_name(CCosControlSocket::KeyPrefix(command_.GetToPath()),
			fz::to_utf8(command_.GetToFile()));
	}

	virtual int Send() override
	{
		rr_ = std::make_shared<CosRequestResponse>();

		if (!step_) {
			std::map<std::string, std::string> headers;
			headers["x-cos-copy-source"] = controlSocket_.host_ + "/" + source_key_;
			if (!controlSocket_.Submit(rr_, "PUT", dest_key_, {}, headers, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		if (!controlSocket_.Submit(rr_, "DELETE", source_key_, {}, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnCosResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Renaming file failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (!step_) {
			step_ = 1;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}
		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	CRenameCommand command_;
	std::string source_key_;
	std::string dest_key_;
	int step_{};
	std::shared_ptr<CosRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CCosControlSocket
// ---------------------------------------------------------------------------

CCosControlSocket::CCosControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	client_.emplace(*this);
}

CCosControlSocket::~CCosControlSocket()
{
	client_.reset();
}

void CCosControlSocket::ResetSocket()
{
	tls_layer_.reset();
	CRealControlSocket::ResetSocket();
}

void CCosControlSocket::Connect(CServer const& server, Credentials const& credentials)
{
	currentServer_ = server;
	credentials_ = credentials;

	host_ = fz::to_utf8(server.GetHost());
	cred_.secret_id = fz::to_utf8(server.GetUser());
	cred_.secret_key = fz::to_utf8(credentials.GetPass());
	cred_.security_token = fz::to_utf8(server.GetExtraParameter("sts_token"));

	Push(std::make_unique<CCosConnectOpData>(*this));
}

std::string CCosControlSocket::KeyPrefix(CServerPath const& path)
{
	std::string full = fz::to_utf8(path.GetPath());
	while (!full.empty() && full.front() == '/') {
		full.erase(0, 1);
	}
	return full;
}

bool CCosControlSocket::Submit(std::shared_ptr<CosRequestResponse> const& rr, std::string const& verb,
	std::string const& key_path, std::vector<std::pair<std::string, std::string>> query,
	std::map<std::string, std::string> extra_headers, CCosRequestHandler * handler)
{
	if (!client_ || !rr) {
		return false;
	}

	auto & req = rr->request_;
	req.verb_ = verb;
	req.uri_.scheme_ = "https";
	req.uri_.host_ = host_;
	req.uri_.path_ = "/" + tcos::encode_key(key_path);

	std::string query_string;
	for (auto const& q : query) {
		if (!query_string.empty()) {
			query_string += '&';
		}
		query_string += encode_query_component(q.first);
		if (!q.second.empty()) {
			query_string += '=';
			query_string += encode_query_component(q.second);
		}
	}
	req.uri_.query_ = query_string;

	std::map<std::string, std::string> headers = extra_headers;
	headers["Host"] = host_;
	if (!cred_.security_token.empty()) {
		headers["x-cos-security-token"] = cred_.security_token;
	}

	auto const signed_req = tcos::sign(cred_, verb, req.uri_.path_, query, 
		[&headers]() {
			std::vector<std::pair<std::string, std::string>> v;
			for (auto const& h : headers) {
				v.emplace_back(h.first, h.second);
			}
			return v;
		}(), tcos::key_time_now());

	for (auto const& h : headers) {
		req.headers_[h.first] = h.second;
	}
	req.headers_["Authorization"] = signed_req.authorization;

	pending_handler_ = handler;
	if (!client_->add_request(rr)) {
		pending_handler_ = nullptr;
		return false;
	}
	return true;
}

void CCosControlSocket::OnRequestDone(uint64_t, bool success)
{
	auto * handler = pending_handler_;
	pending_handler_ = nullptr;
	if (handler) {
		handler->OnCosResponse(success);
	}
}

void CCosControlSocket::OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info)
{
	if (!tls_layer_ || source != tls_layer_.get()) {
		return;
	}
	SendAsyncRequest(std::make_unique<CCertificateNotification>(std::move(info)));
}

void CCosControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::certificate_verification_event, fz::http::client::done_event>(ev, this,
			&CCosControlSocket::OnVerifyCert, &CCosControlSocket::OnRequestDone))
	{
		return;
	}
	CRealControlSocket::operator()(ev);
}

void CCosControlSocket::List(CServerPath const& path, int)
{
	Push(std::make_unique<CCosListOpData>(*this, path));
}

void CCosControlSocket::FileTransfer(CFileTransferCommand const& cmd)
{
	Push(std::make_unique<CCosFileTransferOpData>(*this, cmd));
}

void CCosControlSocket::Delete(CServerPath const& path, std::vector<std::wstring>&& files)
{
	Push(std::make_unique<CCosDeleteOpData>(*this, path, std::move(files)));
}

void CCosControlSocket::RemoveDir(CServerPath const&, std::wstring const&)
{
	log(logmsg::error, _("Removing directories is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

void CCosControlSocket::Mkdir(CServerPath const& path, transfer_flags const&)
{
	Push(std::make_unique<CCosMkdirOpData>(*this, path));
}

void CCosControlSocket::Rename(CRenameCommand const& command)
{
	Push(std::make_unique<CCosRenameOpData>(*this, command));
}

void CCosControlSocket::Chmod(CChmodCommand const&)
{
	log(logmsg::status, _("Setting permissions is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

int CCosControlSocket::DoClose(int nErrorCode)
{
	pending_handler_ = nullptr;
	if (client_) {
		client_->stop(true);
	}
	tls_layer_.reset();

	return CRealControlSocket::DoClose(nErrorCode);
}

bool CCosControlSocket::SetAsyncRequestReply(CAsyncRequestNotification * pNotification)
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
