#include "../filezilla.h"

#include "s3controlsocket.h"
#include "s3response.h"

#include "../controlsocket.h"
#include "../directorycache.h"
#include "../engineprivate.h"
#include "../tls.h"

#include "../../include/directorylisting.h"
#include "../../include/engine_context.h"
#include "../../include/engine_options.h"
#include "../../include/notification.h"

#include <libfilezilla/encode.hpp>
#include <libfilezilla/tls_layer.hpp>
#include <libfilezilla/tls_system_trust_store.hpp>

#include <algorithm>

using namespace std::literals;

namespace {

std::wstring to_wstring(std::string const& in)
{
	return fz::to_wstring_from_utf8(in);
}

std::string ensure_trailing_slash(std::string in)
{
	if (!in.empty() && in.back() != '/') {
		in += '/';
	}
	return in;
}

/// Converts an object key relative to the listing prefix into a display name.
std::wstring key_to_name(std::string const& key, std::string const& prefix, bool & is_dir)
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
// s3_http_client
// ---------------------------------------------------------------------------

s3_http_client::s3_http_client(CS3ControlSocket & owner)
	: fz::http::client::client(owner, *owner.buffer_pool_, owner.logger(), "FileZilla/"s + ENGINE_VERSION)
	, owner_(owner)
{
}

s3_http_client::~s3_http_client()
{
	destroy();
}

fz::socket_interface* s3_http_client::create_socket(fz::native_string const& host, unsigned short, bool tls)
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

void s3_http_client::destroy_socket()
{
	owner_.ResetSocket();
}

void s3_http_client::on_alive()
{
	owner_.SetAlive();
}

// ---------------------------------------------------------------------------
// CS3ConnectOpData
// ---------------------------------------------------------------------------

class CS3ConnectOpData final : public COpData, public CS3OpData
{
public:
	explicit CS3ConnectOpData(CS3ControlSocket & controlSocket)
		: COpData(Command::connect, L"CS3ConnectOpData")
		, CS3OpData(controlSocket)
	{}

	virtual int Send() override { return FZ_REPLY_OK; }
	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }
};

// ---------------------------------------------------------------------------
// CS3ListOpData
// ---------------------------------------------------------------------------

class CS3ListOpData final : public COpData, public CS3OpData, public CS3RequestHandler
{
public:
	CS3ListOpData(CS3ControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::list, L"CS3ListOpData")
		, CS3OpData(controlSocket)
		, path_(path)
	{
		auto split = CS3ControlSocket::SplitPath(path_);
		bucket_ = split.first;
		prefix_ = split.second;
	}

	virtual int Send() override
	{
		if (!Build()) {
			log(logmsg::error, L"Could not create request");
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnS3Response(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log_error();
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());

		if (bucket_.empty()) {
			FinishBuckets(body);
			return;
		}

		s3::list_result result;
		if (!s3::parse_list_objects(body, result)) {
			log(logmsg::error, _("Could not parse the server's response."));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		for (auto const& e : result.entries) {
			bool is_dir = e.is_prefix;
			std::wstring const name = key_to_name(e.key, prefix_, is_dir);
			if (name.empty()) {
				continue;
			}

			CDirentry entry;
			entry.name = name;
			entry.size = is_dir ? -1 : static_cast<int64_t>(e.size);
			entry.flags = is_dir ? CDirentry::flag_dir : 0;
			entry.time.set_rfc3339(e.last_modified);
			entries_.push_back(std::move(entry));
		}

		if (result.truncated && !result.next_continuation_token.empty() && pages_ < 10000) {
			++pages_;
			continuation_ = result.next_continuation_token;
			rr_.reset();
			if (!Build()) {
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			}
			return;
		}

		Finish();
	}

private:
	bool Build()
	{
		rr_ = std::make_shared<S3RequestResponse>();

		std::vector<std::pair<std::string, std::string>> query;
		std::string payload_hash = s3::sha256_hex("");

		std::string raw_path;
		if (bucket_.empty()) {
			raw_path = "/";
		}
		else {
			raw_path = CS3ControlSocket::BucketPath(path_);
			query.emplace_back("list-type", "2");
			query.emplace_back("delimiter", "/");
			query.emplace_back("max-keys", "1000");
			if (!prefix_.empty()) {
				query.emplace_back("prefix", prefix_);
			}
			if (!continuation_.empty()) {
				query.emplace_back("continuation-token", continuation_);
			}
		}

		controlSocket_.FillRequest(rr_->request_, "GET", raw_path, query, payload_hash);
		return controlSocket_.Submit(rr_, this);
	}

	void FinishBuckets(std::string const& body)
	{
		std::vector<s3::bucket_entry> buckets;
		if (!s3::parse_list_buckets(body, buckets)) {
			log(logmsg::error, _("Could not parse the server's response."));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		for (auto const& b : buckets) {
			CDirentry entry;
			entry.name = to_wstring(b.name);
			entry.size = -1;
			entry.flags = CDirentry::flag_dir;
			entry.time.set_rfc3339(b.creation_date);
			entries_.push_back(std::move(entry));
		}

		Finish();
	}

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
		auto const code = rr_ ? rr_->response_.code_ : 0;
		s3::error_info info;
		if (rr_ && s3::parse_error(std::string(rr_->response_.body_.to_view()), info) && !info.code.empty()) {
			log(logmsg::error, _("Server error: %s (%s)"), to_wstring(info.code), to_wstring(info.message));
		}
		else {
			log(logmsg::error, _("Server returned HTTP status %d"), static_cast<int>(code));
		}
	}

	std::shared_ptr<S3RequestResponse> rr_;
	CServerPath path_;
	std::string bucket_;
	std::string prefix_;
	std::string continuation_;
	std::vector<CDirentry> entries_;
	int pages_{};
};

// ---------------------------------------------------------------------------
// CS3FileTransferOpData
// ---------------------------------------------------------------------------

class CS3FileTransferOpData final : public CFileTransferOpData, public CS3OpData, public CS3RequestHandler
{
public:
	CS3FileTransferOpData(CS3ControlSocket & controlSocket, CFileTransferCommand const& cmd)
		: CFileTransferOpData(L"CS3FileTransferOpData", cmd)
		, CS3OpData(controlSocket)
	{}

	virtual int Send() override
	{
		for (;;) {
			switch (stage_) {
			case stage_start:
			{
				raw_path_ = CS3ControlSocket::ObjectPath(remotePath_, remoteFile_);

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

				// Multipart uploads need a known size and a seekable reader so each
				// part can be opened at its own offset.
				upload_size_ = reader_factory_->size();
				multipart_ = reader_factory_->seekable() &&
					upload_size_ != fz::aio_base::nosize &&
					upload_size_ > part_size_;

				if (multipart_) {
					part_count_ = static_cast<int>((upload_size_ + part_size_ - 1) / part_size_);
					log(logmsg::status, _("Uploading %s in %d parts"),
						to_wstring(fz::to_string(upload_size_)), part_count_);
				}

				stage_ = multipart_ ? stage_initiate : stage_single_put;
				continue;
			}
			case stage_download:
			{
				rr_ = std::make_shared<S3RequestResponse>();
				controlSocket_.FillRequest(rr_->request_, "GET", raw_path_, {}, s3::sha256_hex(""));
				rr_->set_on_header([this](std::shared_ptr<S3RequestResponse> const& srr) {
					return OnHeader(srr);
				});
				if (!controlSocket_.Submit(rr_, this)) {
					log(logmsg::error, _("Could not submit the request."));
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_single_put:
			{
				rr_ = std::make_shared<S3RequestResponse>();
				auto & req = rr_->request_;
				req.body_ = reader_factory_->open(*controlSocket_.buffer_pool_, 0, fz::aio_base::nosize,
					controlSocket_.max_buffer_count());
				if (!req.body_) {
					log(logmsg::error, _("Could not open the local file."));
					return FZ_REPLY_ERROR;
				}

				// The body is streamed, so its hash is unknown up front.
				controlSocket_.FillRequest(req, "PUT", raw_path_, {}, "UNSIGNED-PAYLOAD");
				req.update_content_length_from_body();

				if (!controlSocket_.Submit(rr_, this)) {
					log(logmsg::error, _("Could not submit the request."));
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_initiate:
			{
				rr_ = std::make_shared<S3RequestResponse>();
				controlSocket_.FillRequest(rr_->request_, "POST", raw_path_,
					{{"uploads", ""}}, s3::sha256_hex(""));
				if (!controlSocket_.Submit(rr_, this)) {
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

				rr_ = std::make_shared<S3RequestResponse>();
				auto & req = rr_->request_;
				req.body_ = reader_factory_->open(*controlSocket_.buffer_pool_, offset, this_size,
					controlSocket_.max_buffer_count());
				if (!req.body_) {
					log(logmsg::error, _("Could not open the local file."));
					return FZ_REPLY_ERROR;
				}

				controlSocket_.FillRequest(req, "PUT", raw_path_,
					{{"partNumber", std::to_string(current_part_)}, {"uploadId", upload_id_}},
					"UNSIGNED-PAYLOAD");
				req.update_content_length_from_body();

				if (!controlSocket_.Submit(rr_, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_complete:
			{
				rr_ = std::make_shared<S3RequestResponse>();
				std::string const body = s3::build_complete_multipart(parts_);
				rr_->request_.body_ = fz::string_reader_factory(L"complete", body).open(*controlSocket_.buffer_pool_);
				if (!rr_->request_.body_) {
					return FZ_REPLY_ERROR;
				}
				controlSocket_.FillRequest(rr_->request_, "POST", raw_path_,
					{{"uploadId", upload_id_}}, s3::sha256_hex(body));
				rr_->request_.update_content_length_from_body();

				if (!controlSocket_.Submit(rr_, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			case stage_abort:
			{
				rr_ = std::make_shared<S3RequestResponse>();
				controlSocket_.FillRequest(rr_->request_, "DELETE", raw_path_,
					{{"uploadId", upload_id_}}, s3::sha256_hex(""));
				if (!controlSocket_.Submit(rr_, this)) {
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

	virtual void OnS3Response(bool success) override
	{
		if (stage_ == stage_abort) {
			// Best effort cleanup, the transfer itself already failed
			log(logmsg::debug_warning, L"Multipart upload aborted");
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (!rr_ || !success || !rr_->response_.success()) {
			log_error();
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
		{
			if (!s3::parse_initiate_multipart(std::string(rr_->response_.body_.to_view()), upload_id_)) {
				log(logmsg::error, _("Could not parse the server's response."));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			current_part_ = 1;
			stage_ = stage_part;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}
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
		case stage_complete:
			log(logmsg::status, _("File transfer successful"));
			controlSocket_.ResetOperation(FZ_REPLY_OK);
			return;
		default:
		{
			int64_t transferred = 0;
			if (auto const contentLength = rr_->response_.get_header("content-length"); !contentLength.empty()) {
				transferred = fz::to_integral<int64_t>(contentLength, 0);
			}
			controlSocket_.RecordActivity(download() ? activity_logger::recv : activity_logger::send, transferred);

			log(logmsg::status, _("File transfer successful"));
			controlSocket_.ResetOperation(FZ_REPLY_OK);
			return;
		}
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

	void log_error()
	{
		s3::error_info info;
		if (rr_ && s3::parse_error(std::string(rr_->response_.body_.to_view()), info) && !info.code.empty()) {
			log(logmsg::error, _("Server error: %s (%s)"), to_wstring(info.code), to_wstring(info.message));
		}
		else {
			log(logmsg::error, _("File transfer failed"));
		}
	}

	fz::http::continuation OnHeader(std::shared_ptr<S3RequestResponse> const& srr)
	{
		auto const code = srr->response_.code_;
		if (!download()) {
			return fz::http::continuation::next;
		}

		if (code == 416 && resume_) {
			log(logmsg::debug_info, L"Server does not support resuming, restarting transfer");
			resume_ = false;
			return fz::http::continuation::error;
		}

		if (code != 200 && code != 206) {
			// Error body is collected in the response buffer and reported in OnS3Response
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

	std::shared_ptr<S3RequestResponse> rr_;
	std::string raw_path_;
	std::string upload_id_;
	std::vector<s3::part_entry> parts_;
	uint64_t part_size_{8 * 1024 * 1024};
	uint64_t upload_size_{};
	int part_count_{};
	int current_part_{};
	int stage_{stage_start};
	bool multipart_{};
};


// ---------------------------------------------------------------------------
// CS3DeleteOpData
// ---------------------------------------------------------------------------

class CS3DeleteOpData final : public COpData, public CS3OpData, public CS3RequestHandler
{
public:
	CS3DeleteOpData(CS3ControlSocket & controlSocket, CServerPath const& path, std::vector<std::wstring> files)
		: COpData(Command::del, L"CS3DeleteOpData")
		, CS3OpData(controlSocket)
		, path_(path)
		, files_(std::move(files))
	{}

	virtual int Send() override
	{
		if (files_.empty()) {
			return FZ_REPLY_OK;
		}

		current_ = files_.back();
		rr_ = std::make_shared<S3RequestResponse>();
		controlSocket_.FillRequest(rr_->request_, "DELETE",
			CS3ControlSocket::ObjectPath(path_, current_), {}, s3::sha256_hex(""));

		if (!controlSocket_.Submit(rr_, this)) {
			return FZ_REPLY_ERROR;
		}

		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnS3Response(bool success) override
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

		// Continue with the next file
		controlSocket_.SendNextCommand();
	}

private:
	CServerPath path_;
	std::vector<std::wstring> files_;
	std::wstring current_;
	std::shared_ptr<S3RequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CS3MkdirOpData
// ---------------------------------------------------------------------------

class CS3MkdirOpData final : public COpData, public CS3OpData, public CS3RequestHandler
{
public:
	CS3MkdirOpData(CS3ControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::mkdir, L"CS3MkdirOpData")
		, CS3OpData(controlSocket)
		, path_(path)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<S3RequestResponse>();
		// Object stores have no real directories: create a zero-byte placeholder
		// object whose key ends in '/'.
		controlSocket_.FillRequest(rr_->request_, "PUT",
			CS3ControlSocket::FolderPath(path_), {}, s3::sha256_hex(""));

		if (!controlSocket_.Submit(rr_, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnS3Response(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Creating directory \"%s\" failed"), path_.GetPath());
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	CServerPath path_;
	std::shared_ptr<S3RequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CS3RenameOpData (copy + delete)
// ---------------------------------------------------------------------------

class CS3RenameOpData final : public COpData, public CS3OpData, public CS3RequestHandler
{
public:
	CS3RenameOpData(CS3ControlSocket & controlSocket, CRenameCommand const& command)
		: COpData(Command::rename, L"CS3RenameOpData")
		, CS3OpData(controlSocket)
		, command_(command)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<S3RequestResponse>();

		auto const & from = command_.GetFromPath();
		auto const & to = command_.GetToPath();
		std::string const source = CS3ControlSocket::ObjectPath(from, command_.GetFromFile());
		std::string const target = CS3ControlSocket::ObjectPath(to, command_.GetToFile());

		std::map<std::string, std::string> headers;
		headers["x-amz-copy-source"] = source;

		if (!step_) {
			controlSocket_.FillRequest(rr_->request_, "PUT", target, {}, s3::sha256_hex(""), headers);
		}
		else {
			controlSocket_.FillRequest(rr_->request_, "DELETE", source, {}, s3::sha256_hex(""));
		}

		if (!controlSocket_.Submit(rr_, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnS3Response(bool success) override
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
	int step_{};
	std::shared_ptr<S3RequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CS3ControlSocket
// ---------------------------------------------------------------------------

CS3ControlSocket::CS3ControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	client_.emplace(*this);
}

CS3ControlSocket::~CS3ControlSocket()
{
	client_.reset();
}

void CS3ControlSocket::ResetSocket()
{
	tls_layer_.reset();
	CRealControlSocket::ResetSocket();
}

void CS3ControlSocket::Connect(CServer const& server, Credentials const& credentials)
{
	currentServer_ = server;
	credentials_ = credentials;

	// The endpoint may also be plain HTTP (MinIO, Ceph, private S3
	// installations, local emulators). The "tls" extra parameter (1/0,
	// true/false, http/https) or a URL typed into the host field decides the
	// scheme, otherwise the port does (443/8443 mean HTTPS).
	unsigned short const default_port = server.GetProtocol() == MINIO ? 9000 : 443;
	unsigned short const configured_port = server.GetPort() ? server.GetPort() : default_port;
	endpoint_ = cloud::resolve(fz::to_utf8(server.GetHost()),
		fz::to_utf8(server.GetExtraParameter("tls")), configured_port);

	host_ = endpoint_.host;
	port_ = endpoint_.port;
	tls_ = endpoint_.tls();

	region_ = fz::to_utf8(server.GetExtraParameter("region"));
	if (region_.empty()) {
		switch (server.GetProtocol()) {
		case CLOUDFLARE_R2:
			// R2 expects the literal region name "auto"
			region_ = "auto";
			break;
		case S3_CN:
			region_ = "cn-north-1";
			break;
		default:
			region_ = "us-east-1";
			break;
		}
	}

	cred_.access_key = fz::to_utf8(server.GetUser());
	cred_.secret_key = fz::to_utf8(credentials.GetPass());

	default_headers_.clear();
	if (auto const sse = fz::to_utf8(server.GetExtraParameter("ssealgorithm")); !sse.empty()) {
		default_headers_["x-amz-server-side-encryption"] = sse;
	}

	// Port in the Host header only when it is not the default for the scheme.
	host_hdr_ = endpoint_.authority();

	Push(std::make_unique<CS3ConnectOpData>(*this));
}

std::pair<std::string, std::string> CS3ControlSocket::SplitPath(CServerPath const& path)
{
	std::string const full = fz::to_utf8(path.GetPath());
	if (full.empty() || full == "/") {
		return {"", ""};
	}

	std::string rest = full;
	if (rest.front() == '/') {
		rest.erase(0, 1);
	}

	std::string bucket;
	std::string prefix;
	auto const pos = rest.find('/');
	if (pos == std::string::npos) {
		bucket = rest;
	}
	else {
		bucket = rest.substr(0, pos);
		prefix = ensure_trailing_slash(rest.substr(pos + 1));
	}

	return {bucket, prefix};
}

std::string CS3ControlSocket::BucketPath(CServerPath const& path)
{
	auto const bucket = SplitPath(path).first;
	if (bucket.empty()) {
		return "/";
	}
	return "/" + bucket;
}

std::string CS3ControlSocket::ObjectPath(CServerPath const& path, std::wstring const& file)
{
	auto const split = SplitPath(path);
	if (split.first.empty()) {
		return "/";
	}

	std::string ret = "/" + split.first + "/" + split.second + fz::to_utf8(file);
	return ret;
}

std::string CS3ControlSocket::FolderPath(CServerPath const& path)
{
	auto const split = SplitPath(path);
	if (split.first.empty()) {
		return "/";
	}

	return "/" + split.first + "/" + split.second;
}

void CS3ControlSocket::FillRequest(S3Request & req, std::string const& verb, std::string const& raw_path,
	std::vector<std::pair<std::string, std::string>> const& query,
	std::string const& payload_hash, std::map<std::string, std::string> extra_headers)
{
	req.verb_ = verb;

	req.uri_.scheme_ = endpoint_.scheme;
	req.uri_.host_ = endpoint_.host;
	req.uri_.port_ = endpoint_.uri_port();
	// fz::uri percent-encodes the path exactly like SigV4 requires, so we keep the
	// decoded form here and use the encoded form only for signing.
	req.uri_.path_ = raw_path;
	req.uri_.query_ = s3::canonical_query_string(query);

	// Headers that must be signed. The HTTP client adds Host on its own, it must
	// match the value we sign.
	s3::signable_request signable;
	signable.method = verb;
	signable.host = host_hdr_;
	signable.canonical_uri = s3::uri_encode(raw_path, true);
	signable.query = query;
	signable.payload_hash = payload_hash;

	auto headers = default_headers_;
	for (auto & h : extra_headers) {
		std::string name = h.first;
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		headers[name] = h.second;
	}
	signable.headers = headers;

	auto const signed_req = s3::sign(signable, cred_, region_, service_, s3::amz_date_now());

	for (auto const& h : headers) {
		req.headers_[h.first] = h.second;
	}
	req.headers_["Host"] = host_hdr_;
	req.headers_["x-amz-date"] = signed_req.amz_date;
	req.headers_["x-amz-content-sha256"] = payload_hash;
	req.headers_["Authorization"] = signed_req.authorization;
	if (!cred_.session_token.empty()) {
		req.headers_["x-amz-security-token"] = cred_.session_token;
	}
}

bool CS3ControlSocket::Submit(std::shared_ptr<S3RequestResponse> const& rr, CS3RequestHandler * handler)
{
	if (!client_ || !rr) {
		return false;
	}

	pending_handler_ = handler;
	if (!client_->add_request(rr)) {
		pending_handler_ = nullptr;
		return false;
	}
	return true;
}

void CS3ControlSocket::OnRequestDone(uint64_t, bool success)
{
	auto * handler = pending_handler_;
	pending_handler_ = nullptr;
	if (handler) {
		handler->OnS3Response(success);
	}
}

void CS3ControlSocket::OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info)
{
	if (!tls_layer_ || source != tls_layer_.get()) {
		return;
	}

	SendAsyncRequest(std::make_unique<CCertificateNotification>(std::move(info)));
}

void CS3ControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::certificate_verification_event, fz::http::client::done_event>(ev, this,
			&CS3ControlSocket::OnVerifyCert, &CS3ControlSocket::OnRequestDone))
	{
		return;
	}

	CRealControlSocket::operator()(ev);
}

void CS3ControlSocket::List(CServerPath const& path, int)
{
	Push(std::make_unique<CS3ListOpData>(*this, path));
}

void CS3ControlSocket::FileTransfer(CFileTransferCommand const& cmd)
{
	Push(std::make_unique<CS3FileTransferOpData>(*this, cmd));
}

void CS3ControlSocket::Delete(CServerPath const& path, std::vector<std::wstring>&& files)
{
	Push(std::make_unique<CS3DeleteOpData>(*this, path, std::move(files)));
}

void CS3ControlSocket::RemoveDir(CServerPath const& path, std::wstring const& subDir)
{
	CServerPath fullPath = path;
	if (!subDir.empty() && !fullPath.ChangePath(subDir)) {
		log(logmsg::error, _("Invalid path"));
		ResetOperation(FZ_REPLY_ERROR);
		return;
	}

	// Object stores have no atomic directory removal. Listing and removing all
	// contained objects is not implemented yet, report it instead of pretending.
	log(logmsg::error, _("Removing directories is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

void CS3ControlSocket::Mkdir(CServerPath const& path, transfer_flags const&)
{
	Push(std::make_unique<CS3MkdirOpData>(*this, path));
}

void CS3ControlSocket::Rename(CRenameCommand const& command)
{
	Push(std::make_unique<CS3RenameOpData>(*this, command));
}

void CS3ControlSocket::Chmod(CChmodCommand const&)
{
	log(logmsg::status, _("Setting permissions is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

int CS3ControlSocket::DoClose(int nErrorCode)
{
	pending_handler_ = nullptr;
	if (client_) {
		client_->stop(tls_);
	}
	tls_layer_.reset();

	return CRealControlSocket::DoClose(nErrorCode);
}

bool CS3ControlSocket::SetAsyncRequestReply(CAsyncRequestNotification * pNotification)
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
