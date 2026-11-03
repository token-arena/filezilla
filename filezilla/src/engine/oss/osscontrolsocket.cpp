#include "../filezilla.h"

#include "osscontrolsocket.h"

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
// oss_http_client
// ---------------------------------------------------------------------------

oss_http_client::oss_http_client(COssControlSocket & owner)
	: fz::http::client::client(owner, *owner.buffer_pool_, owner.logger(), "FileZilla/"s + ENGINE_VERSION)
	, owner_(owner)
{
}

oss_http_client::~oss_http_client()
{
	destroy();
}

fz::socket_interface* oss_http_client::create_socket(fz::native_string const& host, unsigned short, bool tls)
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

void oss_http_client::destroy_socket()
{
	owner_.ResetSocket();
}

void oss_http_client::on_alive()
{
	owner_.SetAlive();
}

// ---------------------------------------------------------------------------
// COssConnectOpData
// ---------------------------------------------------------------------------

class COssConnectOpData final : public COpData, public COssOpData
{
public:
	explicit COssConnectOpData(COssControlSocket & controlSocket)
		: COpData(Command::connect, L"COssConnectOpData")
		, COssOpData(controlSocket)
	{}

	virtual int Send() override
	{
		if (controlSocket_.cred_.access_key.empty() || controlSocket_.cred_.secret_key.empty()) {
			log(logmsg::error, _("An access key id and an access key secret are required."));
			return FZ_REPLY_CRITICALERROR;
		}
		if (controlSocket_.host_.empty()) {
			log(logmsg::error, _("A regional endpoint (for example oss-cn-hangzhou.aliyuncs.com) is required as host."));
			return FZ_REPLY_CRITICALERROR;
		}
		return FZ_REPLY_OK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }
};

// ---------------------------------------------------------------------------
// COssListOpData
// ---------------------------------------------------------------------------

class COssListOpData final : public COpData, public COssOpData, public COssRequestHandler
{
public:
	COssListOpData(COssControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::list, L"COssListOpData")
		, COssOpData(controlSocket)
		, path_(path)
	{
		auto split = COssControlSocket::SplitPath(path_);
		bucket_ = split.first;
		prefix_ = split.second;
	}

	virtual int Send() override
	{
		rr_ = std::make_shared<OssRequestResponse>();

		std::vector<std::pair<std::string, std::string>> query;
		if (!bucket_.empty()) {
			query.emplace_back("prefix", prefix_);
			query.emplace_back("delimiter", "/");
			query.emplace_back("max-keys", "1000");
			if (!marker_.empty()) {
				query.emplace_back("marker", marker_);
			}
		}

		if (!controlSocket_.Submit(rr_, "GET", bucket_, "", query, {}, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnOssResponse(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log_error();
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());

		if (bucket_.empty()) {
			std::vector<oss::bucket_entry> buckets;
			if (!oss::parse_list_buckets(body, buckets)) {
				log(logmsg::error, _("Could not parse the server's response."));
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
			Finish();
			return;
		}

		oss::list_result result;
		if (!oss::parse_list_objects(body, result)) {
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

	void log_error()
	{
		std::string code, message;
		if (rr_ && oss::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
			log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
		}
		else {
			log(logmsg::error, _("Server returned HTTP status %d"), static_cast<int>(rr_ ? rr_->response_.code_ : 0));
		}
	}

	std::shared_ptr<OssRequestResponse> rr_;
	CServerPath path_;
	std::string bucket_;
	std::string prefix_;
	std::string marker_;
	std::vector<CDirentry> entries_;
	int pages_{};
};

// ---------------------------------------------------------------------------
// COssFileTransferOpData
// ---------------------------------------------------------------------------

class COssFileTransferOpData final : public CFileTransferOpData, public COssOpData, public COssRequestHandler
{
public:
	COssFileTransferOpData(COssControlSocket & controlSocket, CFileTransferCommand const& cmd)
		: CFileTransferOpData(L"COssFileTransferOpData", cmd)
		, COssOpData(controlSocket)
	{}

	virtual int Send() override
	{
		for (;;) {
			switch (stage_) {
			case stage_start:
			{
				auto split = COssControlSocket::SplitPath(remotePath_);
				bucket_ = split.first;
				key_ = oss::join_name(split.second, fz::to_utf8(remoteFile_));
				if (bucket_.empty() || key_.empty()) {
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
				rr_ = std::make_shared<OssRequestResponse>();
				std::map<std::string, std::string> headers;
				if (resume_ && localFileSize_ != 0 && localFileSize_ != fz::aio_base::nosize) {
					headers["Range"] = "bytes=" + std::to_string(localFileSize_) + "-";
				}
				rr_->set_on_header([this](std::shared_ptr<OssRequestResponse> const& srr) { return OnHeader(srr); });
				if (!controlSocket_.Submit(rr_, "GET", bucket_, key_, {}, {}, headers, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_single_put:
			{
				rr_ = std::make_shared<OssRequestResponse>();
				rr_->request_.body_ = reader_factory_->open(*controlSocket_.buffer_pool_, 0, fz::aio_base::nosize,
					controlSocket_.max_buffer_count());
				if (!rr_->request_.body_) {
					log(logmsg::error, _("Could not open the local file."));
					return FZ_REPLY_ERROR;
				}

				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/octet-stream";
				if (!controlSocket_.Submit(rr_, "PUT", bucket_, key_, {}, {}, headers, this)) {
					return FZ_REPLY_ERROR;
				}
				rr_->request_.update_content_length_from_body();
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_initiate:
			{
				rr_ = std::make_shared<OssRequestResponse>();
				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/octet-stream";
				if (!controlSocket_.Submit(rr_, "POST", bucket_, key_, {}, {{"uploads", ""}}, headers, this)) {
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

				rr_ = std::make_shared<OssRequestResponse>();
				rr_->request_.body_ = reader_factory_->open(*controlSocket_.buffer_pool_, offset, this_size,
					controlSocket_.max_buffer_count());
				if (!rr_->request_.body_) {
					log(logmsg::error, _("Could not open the local file."));
					return FZ_REPLY_ERROR;
				}

				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/octet-stream";
				std::vector<std::pair<std::string, std::string>> subs = {
					{"partNumber", std::to_string(current_part_)}, {"uploadId", upload_id_}};

				if (!controlSocket_.Submit(rr_, "PUT", bucket_, key_, {}, subs, headers, this)) {
					return FZ_REPLY_ERROR;
				}
				rr_->request_.update_content_length_from_body();
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_complete:
			{
				rr_ = std::make_shared<OssRequestResponse>();
				std::string const body = oss::build_complete_multipart(parts_);
				rr_->request_.body_ = fz::string_reader_factory(L"complete", body).open(*controlSocket_.buffer_pool_);
				if (!rr_->request_.body_) {
					return FZ_REPLY_ERROR;
				}
				std::map<std::string, std::string> headers;
				headers["Content-Type"] = "application/xml";
				if (!controlSocket_.Submit(rr_, "POST", bucket_, key_, {}, {{"uploadId", upload_id_}}, headers, this)) {
					return FZ_REPLY_ERROR;
				}
				rr_->request_.update_content_length_from_body();
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_abort:
			{
				rr_ = std::make_shared<OssRequestResponse>();
				if (!controlSocket_.Submit(rr_, "DELETE", bucket_, key_, {}, {{"uploadId", upload_id_}}, {}, this)) {
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

	virtual void OnOssResponse(bool success) override
	{
		if (stage_ == stage_abort) {
			log(logmsg::debug_warning, L"Multipart upload aborted");
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (!rr_ || !success || !rr_->response_.success()) {
			std::string code, message;
			if (rr_ && oss::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
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
			if (!oss::parse_initiate_multipart(std::string(rr_->response_.body_.to_view()), upload_id_)) {
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

	fz::http::continuation OnHeader(std::shared_ptr<OssRequestResponse> const& srr)
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

	std::shared_ptr<OssRequestResponse> rr_;
	std::string bucket_;
	std::string key_;
	std::string upload_id_;
	std::vector<oss::part_entry> parts_;
	uint64_t part_size_{8 * 1024 * 1024};
	uint64_t upload_size_{};
	int part_count_{};
	int current_part_{};
	int stage_{stage_start};
	bool multipart_{};
};

// ---------------------------------------------------------------------------
// COssDeleteOpData
// ---------------------------------------------------------------------------

class COssDeleteOpData final : public COpData, public COssOpData, public COssRequestHandler
{
public:
	COssDeleteOpData(COssControlSocket & controlSocket, CServerPath const& path, std::vector<std::wstring> files)
		: COpData(Command::del, L"COssDeleteOpData")
		, COssOpData(controlSocket)
		, path_(path)
		, files_(std::move(files))
	{
		auto split = COssControlSocket::SplitPath(path_);
		bucket_ = split.first;
		prefix_ = split.second;
	}

	virtual int Send() override
	{
		if (files_.empty()) {
			return FZ_REPLY_OK;
		}

		rr_ = std::make_shared<OssRequestResponse>();
		current_ = files_.back();
		std::string const key = oss::join_name(prefix_, fz::to_utf8(current_));

		if (!controlSocket_.Submit(rr_, "DELETE", bucket_, key, {}, {}, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnOssResponse(bool success) override
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
	std::shared_ptr<OssRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// COssMkdirOpData
// ---------------------------------------------------------------------------

class COssMkdirOpData final : public COpData, public COssOpData, public COssRequestHandler
{
public:
	COssMkdirOpData(COssControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::mkdir, L"COssMkdirOpData")
		, COssOpData(controlSocket)
		, path_(path)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<OssRequestResponse>();

		auto split = COssControlSocket::SplitPath(path_);
		bucket_ = split.first;
		std::string name = split.second;
		if (bucket_.empty() || name.empty()) {
			log(logmsg::error, _("Invalid path"));
			return FZ_REPLY_ERROR;
		}
		if (name.back() != '/') {
			name += '/';
		}

		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/octet-stream";
		headers["Content-Length"] = "0";

		if (!controlSocket_.Submit(rr_, "PUT", bucket_, name, {}, {}, headers, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnOssResponse(bool success) override
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
	std::shared_ptr<OssRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// COssRenameOpData (server side copy + delete)
// ---------------------------------------------------------------------------

class COssRenameOpData final : public COpData, public COssOpData, public COssRequestHandler
{
public:
	COssRenameOpData(COssControlSocket & controlSocket, CRenameCommand const& command)
		: COpData(Command::rename, L"COssRenameOpData")
		, COssOpData(controlSocket)
		, command_(command)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<OssRequestResponse>();

		auto from = COssControlSocket::SplitPath(command_.GetFromPath());
		auto to = COssControlSocket::SplitPath(command_.GetToPath());
		source_key_ = oss::join_name(from.second, fz::to_utf8(command_.GetFromFile()));
		std::string const target_key = oss::join_name(to.second, fz::to_utf8(command_.GetToFile()));

		if (step_ == 0) {
			std::map<std::string, std::string> headers;
			headers["x-oss-copy-source"] = "/" + from.first + "/" + source_key_;
			if (!controlSocket_.Submit(rr_, "PUT", to.first, target_key, {}, {}, headers, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		if (!controlSocket_.Submit(rr_, "DELETE", from.first, source_key_, {}, {}, {}, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnOssResponse(bool success) override
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
	std::string source_key_;
	int step_{};
	std::shared_ptr<OssRequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// COssControlSocket
// ---------------------------------------------------------------------------

COssControlSocket::COssControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	client_.emplace(*this);
}

COssControlSocket::~COssControlSocket()
{
	client_.reset();
}

void COssControlSocket::ResetSocket()
{
	tls_layer_.reset();
	CRealControlSocket::ResetSocket();
}

void COssControlSocket::Connect(CServer const& server, Credentials const& credentials)
{
	currentServer_ = server;
	credentials_ = credentials;

	host_ = fz::to_utf8(server.GetHost());
	cred_.access_key = fz::to_utf8(server.GetUser());
	cred_.secret_key = fz::to_utf8(credentials.GetPass());
	cred_.security_token = fz::to_utf8(server.GetExtraParameter("sts_token"));

	// V4 signatures are scoped to a region: take it from the "region" extra
	// parameter, or derive it from the endpoint host name.
	region_ = fz::to_utf8(server.GetExtraParameter("region"));
	if (region_.empty()) {
		region_ = oss::region_from_endpoint(host_);
	}
	if (region_.size() > 4 && region_.compare(0, 4, "oss-") == 0) {
		region_ = region_.substr(4);
	}

	version_ = oss::parse_signature_version(fz::to_utf8(server.GetExtraParameter("signature_version")));
	effective_version_ = version_;
	fallback_done_ = false;

	// V4 needs a region for the credential scope. When it cannot be derived (for
	// example with a CNAME endpoint and no "region" parameter) fall back to V1,
	// which does not need one.
	if (version_ != oss::signature_version::v1 && region_.empty()) {
		log(logmsg::status, L"Could not determine the OSS region from the endpoint, using signature version 1");
		effective_version_ = oss::signature_version::v1;
		version_ = oss::signature_version::v1;
	}

	Push(std::make_unique<COssConnectOpData>(*this));
}

std::pair<std::string, std::string> COssControlSocket::SplitPath(CServerPath const& path)
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

bool COssControlSocket::Submit(std::shared_ptr<OssRequestResponse> const& rr, std::string const& verb,
	std::string const& bucket, std::string const& key,
	std::vector<std::pair<std::string, std::string>> query,
	std::vector<std::pair<std::string, std::string>> subresources,
	std::map<std::string, std::string> extra_headers, COssRequestHandler * handler)
{
	request_recipe recipe;
	recipe.verb = verb;
	recipe.bucket = bucket;
	recipe.key = key;
	recipe.query = std::move(query);
	recipe.subresources = std::move(subresources);
	recipe.headers = std::move(extra_headers);
	recipe.has_body = rr && rr->request_.body_ != nullptr;

	pending_recipe_ = recipe;
	pending_rr_ = rr;

	if (version_ == oss::signature_version::auto_fallback) {
		// Always start with V4, which is what Alibaba recommends.
		effective_version_ = oss::signature_version::v4;
		fallback_done_ = false;
	}

	return FillRequest(rr, recipe, effective_version_, handler);
}

bool COssControlSocket::FillRequest(std::shared_ptr<OssRequestResponse> const& rr, request_recipe const& recipe,
	oss::signature_version version, COssRequestHandler * handler)
{
	if (!client_ || !rr) {
		return false;
	}

	auto & req = rr->request_;
	req.verb_ = recipe.verb;
	req.uri_.scheme_ = "https";
	req.uri_.host_ = host_;

	std::string path = "/";
	if (!recipe.bucket.empty()) {
		path += recipe.bucket + "/";
		path += oss::encode_key(recipe.key);
	}
	req.uri_.path_ = path;

	// Sub-resources belong into the URL as well as into the signature; ordinary
	// query parameters are signed by V4 but not by V1. An empty value is sent as
	// a bare key in both cases.
	auto all_query = recipe.query;
	for (auto const& s : recipe.subresources) {
		all_query.push_back(s);
	}

	std::string query_string;
	for (auto const& q : all_query) {
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

	std::map<std::string, std::string> headers = recipe.headers;
	if (!cred_.security_token.empty()) {
		headers["x-oss-security-token"] = cred_.security_token;
	}

	std::string authorization;
	if (version == oss::signature_version::v1) {
		headers["Date"] = oss::rfc1123_now();
		authorization = oss::sign(cred_, recipe.verb, headers, recipe.bucket, recipe.key, recipe.subresources);
	}
	else {
		auto const timestamp = oss::iso8601_now();
		headers["x-oss-date"] = timestamp;
		headers["x-oss-content-sha256"] = "UNSIGNED-PAYLOAD";
		headers["Date"] = oss::rfc1123_now();

		// Content-Length is not among the mandatory signed headers, but uploads
		// carry it, so list it as an additional signed header to make sure the
		// server sees exactly what we signed.
		std::vector<std::string> additional;
		if (headers.count("Content-Length") || headers.count("content-length")) {
			additional.push_back("content-length");
		}

		auto const signed_req = oss::sign_v4(cred_, recipe.verb, path, all_query, headers,
			region_, timestamp, additional);
		authorization = signed_req.authorization;
	}

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


void COssControlSocket::OnRequestDone(uint64_t, bool success)
{
	auto * handler = pending_handler_;

	// With signature_version=auto: when the service rejects the V4 signature,
	// re-send the same request with V1 once. Requests carrying a body cannot be
	// replayed (the reader is consumed), so those simply report the error.
	if (!success && version_ == oss::signature_version::auto_fallback && !fallback_done_ &&
		pending_recipe_ && pending_rr_ && !pending_recipe_->has_body)
	{
		auto & res = pending_rr_->response_;
		std::string code, message;
		oss::parse_error(std::string(res.body_.to_view()), code, message);

		bool const auth_failure = (res.code_ == 400 || res.code_ == 403) &&
			(code == "SignatureDoesNotMatch" || code == "InvalidAccessKeyId" || code == "AccessDenied" ||
			 code == "InvalidArgument" || code == "InvalidRequest" || code == "OperationNotSupported");

		if (auth_failure) {
			fallback_done_ = true;
			effective_version_ = oss::signature_version::v1;
			log(logmsg::status, _("The server rejected the OSS V4 signature (%s), retrying with signature version 1"),
				to_wstring(code.empty() ? message : code));

			auto rr = pending_rr_;
			rr->request_.headers_.clear();
			rr->request_.uri_.clear();
			rr->request_.body_.reset();
			rr->response_.reset();

			auto const recipe = *pending_recipe_;
			if (FillRequest(rr, recipe, effective_version_, handler)) {
				return;
			}
		}
	}

	pending_handler_ = nullptr;
	pending_rr_.reset();
	if (handler) {
		handler->OnOssResponse(success);
	}
}

void COssControlSocket::OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info)
{
	if (!tls_layer_ || source != tls_layer_.get()) {
		return;
	}
	SendAsyncRequest(std::make_unique<CCertificateNotification>(std::move(info)));
}

void COssControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::certificate_verification_event, fz::http::client::done_event>(ev, this,
			&COssControlSocket::OnVerifyCert, &COssControlSocket::OnRequestDone))
	{
		return;
	}
	CRealControlSocket::operator()(ev);
}

void COssControlSocket::List(CServerPath const& path, int)
{
	Push(std::make_unique<COssListOpData>(*this, path));
}

void COssControlSocket::FileTransfer(CFileTransferCommand const& cmd)
{
	Push(std::make_unique<COssFileTransferOpData>(*this, cmd));
}

void COssControlSocket::Delete(CServerPath const& path, std::vector<std::wstring>&& files)
{
	Push(std::make_unique<COssDeleteOpData>(*this, path, std::move(files)));
}

void COssControlSocket::RemoveDir(CServerPath const&, std::wstring const&)
{
	log(logmsg::error, _("Removing directories is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

void COssControlSocket::Mkdir(CServerPath const& path, transfer_flags const&)
{
	Push(std::make_unique<COssMkdirOpData>(*this, path));
}

void COssControlSocket::Rename(CRenameCommand const& command)
{
	Push(std::make_unique<COssRenameOpData>(*this, command));
}

void COssControlSocket::Chmod(CChmodCommand const&)
{
	log(logmsg::status, _("Setting permissions is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

int COssControlSocket::DoClose(int nErrorCode)
{
	pending_handler_ = nullptr;
	if (client_) {
		client_->stop(true);
	}
	tls_layer_.reset();

	return CRealControlSocket::DoClose(nErrorCode);
}

bool COssControlSocket::SetAsyncRequestReply(CAsyncRequestNotification * pNotification)
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
