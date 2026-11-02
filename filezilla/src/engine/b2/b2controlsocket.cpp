#include "../filezilla.h"

#include "b2controlsocket.h"

#include "../controlsocket.h"
#include "../directorycache.h"
#include "../engineprivate.h"
#include "../tls.h"

#include "../../include/directorylisting.h"
#include "../../include/engine_context.h"
#include "../../include/engine_options.h"
#include "../../include/notification.h"

#include <libfilezilla/encode.hpp>
#include <libfilezilla/json.hpp>
#include <libfilezilla/tls_layer.hpp>
#include <libfilezilla/tls_system_trust_store.hpp>
#include <libfilezilla/uri.hpp>

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
// b2_http_client
// ---------------------------------------------------------------------------

b2_http_client::b2_http_client(CB2ControlSocket & owner)
	: fz::http::client::client(owner, *owner.buffer_pool_, owner.logger(), "FileZilla/"s + ENGINE_VERSION)
	, owner_(owner)
{
}

b2_http_client::~b2_http_client()
{
	destroy();
}

fz::socket_interface* b2_http_client::create_socket(fz::native_string const& host, unsigned short, bool tls)
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

void b2_http_client::destroy_socket()
{
	owner_.ResetSocket();
}

void b2_http_client::on_alive()
{
	owner_.SetAlive();
}

// ---------------------------------------------------------------------------
// CB2ConnectOpData
// ---------------------------------------------------------------------------

class CB2ConnectOpData final : public COpData, public CB2OpData, public CB2RequestHandler
{
public:
	explicit CB2ConnectOpData(CB2ControlSocket & controlSocket)
		: COpData(Command::connect, L"CB2ConnectOpData")
		, CB2OpData(controlSocket)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<B2RequestResponse>();

		std::string const user = fz::to_utf8(currentServer_.GetUser());
		std::string const pass = fz::to_utf8(controlSocket_.credentials_.GetPass());
		std::string const basic = fz::base64_encode(user + ":" + pass);

		std::map<std::string, std::string> headers;
		headers["Authorization"] = "Basic " + basic;

		if (!controlSocket_.Submit(rr_, "https://api.backblazeb2.com/b2api/v3/b2_authorize_account",
				"GET", std::string(), headers, this))
		{
			return FZ_REPLY_ERROR;
		}

		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnB2Response(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			std::string code, message;
			if (rr_ && b2::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
				log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
			}
			else {
				log(logmsg::error, _("Authorization failed"));
			}
			controlSocket_.ResetOperation(FZ_REPLY_CRITICALERROR);
			return;
		}

		b2::auth_info info;
		std::string error;
		if (!b2::parse_authorize(std::string(rr_->response_.body_.to_view()), info, error)) {
			log(logmsg::error, _("Could not parse the server's response: %s"), to_wstring(error));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		controlSocket_.api_url_ = info.api_url;
		controlSocket_.download_url_ = info.download_url;
		controlSocket_.auth_token_ = info.authorization_token;
		controlSocket_.account_id_ = info.account_id;
		if (!info.bucket_id.empty() && !info.bucket_name.empty()) {
			controlSocket_.bucket_ids_[info.bucket_name] = info.bucket_id;
		}

		log(logmsg::status, _("Authorized, account %s"), to_wstring(info.account_id));
		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	std::shared_ptr<B2RequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CB2ListOpData
// ---------------------------------------------------------------------------

class CB2ListOpData final : public COpData, public CB2OpData, public CB2RequestHandler
{
public:
	CB2ListOpData(CB2ControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::list, L"CB2ListOpData")
		, CB2OpData(controlSocket)
		, path_(path)
	{
		auto split = CB2ControlSocket::SplitPath(path_);
		bucket_ = split.first;
		prefix_ = split.second;
	}

	virtual int Send() override
	{
		rr_ = std::make_shared<B2RequestResponse>();

		if (bucket_.empty()) {
			// List the buckets themselves
			state_ = state_buckets;
			if (!controlSocket_.ResolveBucket(rr_, std::string(), this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		auto it = controlSocket_.bucket_ids_.find(bucket_);
		if (it == controlSocket_.bucket_ids_.end()) {
			// Look up the id first, then continue in the next step
			state_ = state_resolve;
			if (!controlSocket_.ResolveBucket(rr_, bucket_, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		state_ = state_files;
		std::string const body = b2::make_list_request(it->second, prefix_, "/", 1000, next_file_);
		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/json";
		if (!controlSocket_.Submit(rr_, controlSocket_.ApiUrl() + "/b2api/v3/b2_list_file_names",
				"POST", body, headers, this))
		{
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnB2Response(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log_error();
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());
		std::string error;

		if (state_ == state_buckets) {
			std::vector<b2::bucket_entry> buckets;
			if (!b2::parse_list_buckets(body, buckets, error)) {
				log(logmsg::error, _("Could not parse the server's response: %s"), to_wstring(error));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			for (auto const& b : buckets) {
				controlSocket_.bucket_ids_[b.bucket_name] = b.bucket_id;
				CDirentry entry;
				entry.name = to_wstring(b.bucket_name);
				entry.size = -1;
				entry.flags = CDirentry::flag_dir;
				entries_.push_back(std::move(entry));
			}
			Finish();
			return;
		}

		if (state_ == state_resolve) {
			auto it = controlSocket_.bucket_ids_.find(bucket_);
			if (it == controlSocket_.bucket_ids_.end()) {
				log(logmsg::error, _("Bucket \"%s\" not found"), to_wstring(bucket_));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			rr_.reset();
			state_ = state_files;
			controlSocket_.SendNextCommand();
			return;
		}

		b2::list_result result;
		if (!b2::parse_list_file_names(body, result, error)) {
			log(logmsg::error, _("Could not parse the server's response: %s"), to_wstring(error));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		for (auto const& f : result.files) {
			if (f.action == "hide") {
				continue;
			}
			bool is_dir = f.action == "folder";
			std::wstring const name = name_from_key(f.file_name, prefix_, is_dir);
			if (name.empty()) {
				continue;
			}
			CDirentry entry;
			entry.name = name;
			entry.size = is_dir ? -1 : f.size;
			entry.flags = is_dir ? CDirentry::flag_dir : 0;
			if (f.upload_timestamp > 0) {
				entry.time = fz::datetime(static_cast<time_t>(f.upload_timestamp / 1000), fz::datetime::seconds);
			}
			entries_.push_back(std::move(entry));
		}

		if (result.has_more && pages_ < 10000) {
			++pages_;
			next_file_ = result.next_file_name;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}

		Finish();
	}

private:
	enum states { state_buckets, state_resolve, state_files };

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
		if (rr_ && b2::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
			log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
		}
		else {
			log(logmsg::error, _("Server returned HTTP status %d"), static_cast<int>(rr_ ? rr_->response_.code_ : 0));
		}
	}

	std::shared_ptr<B2RequestResponse> rr_;
	CServerPath path_;
	std::string bucket_;
	std::string prefix_;
	std::string next_file_;
	std::vector<CDirentry> entries_;
	int pages_{};
	int state_{state_files};
};

// ---------------------------------------------------------------------------
// CB2FileTransferOpData
// ---------------------------------------------------------------------------

class CB2FileTransferOpData final : public CFileTransferOpData, public CB2OpData, public CB2RequestHandler
{
public:
	CB2FileTransferOpData(CB2ControlSocket & controlSocket, CFileTransferCommand const& cmd)
		: CFileTransferOpData(L"CB2FileTransferOpData", cmd)
		, CB2OpData(controlSocket)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<B2RequestResponse>();

		auto split = CB2ControlSocket::SplitPath(remotePath_);
		bucket_ = split.first;
		object_name_ = b2::join_name(split.second, fz::to_utf8(remoteFile_));

		if (download()) {
			if (opState == 0) {
				int res = controlSocket_.CheckOverwriteFile();
				if (res != FZ_REPLY_OK) {
					return res;
				}
				opState = 1;
			}

			std::string url = controlSocket_.download_url_ + "/file/" + bucket_ + "/" + b2::encode_file_name(object_name_);
			rr_->set_on_header([this](std::shared_ptr<B2RequestResponse> const& srr) { return OnHeader(srr); });

			std::map<std::string, std::string> headers;
			headers["Authorization"] = controlSocket_.AuthToken();
			if (!controlSocket_.Submit(rr_, url, "GET", std::string(), headers, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		switch (opState) {
		case 0: // resolve bucket id
		{
			auto it = controlSocket_.bucket_ids_.find(bucket_);
			if (it != controlSocket_.bucket_ids_.end()) {
				bucket_id_ = it->second;
				opState = 1;
				controlSocket_.SendNextCommand();
				return FZ_REPLY_WOULDBLOCK;
			}
			if (!controlSocket_.ResolveBucket(rr_, bucket_, this)) {
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}
		case 1: // ask for an upload url
		{
			if (bucket_id_.empty()) {
				auto it = controlSocket_.bucket_ids_.find(bucket_);
				if (it == controlSocket_.bucket_ids_.end()) {
					log(logmsg::error, _("Bucket \"%s\" not found"), to_wstring(bucket_));
					return FZ_REPLY_ERROR;
				}
				bucket_id_ = it->second;
			}

			std::map<std::string, std::string> headers;
			headers["Content-Type"] = "application/json";
			if (!controlSocket_.Submit(rr_, controlSocket_.ApiUrl() + "/b2api/v3/b2_get_upload_url",
					"POST", b2::make_upload_url_request(bucket_id_), headers, this))
			{
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}
		case 2: // actual upload
		{
			if (!reader_factory_) {
				log(logmsg::error, _("No local file to send."));
				return FZ_REPLY_ERROR;
			}
			rr_->request_.body_ = reader_factory_->open(*controlSocket_.buffer_pool_, 0, fz::aio_base::nosize,
				controlSocket_.max_buffer_count());
			if (!rr_->request_.body_) {
				log(logmsg::error, _("Could not open the local file."));
				return FZ_REPLY_ERROR;
			}

			std::map<std::string, std::string> headers;
			headers["Authorization"] = upload_token_;
			headers["X-Bz-File-Name"] = b2::encode_file_name(object_name_);
			headers["Content-Type"] = "b2/x-auto";
			// Verifying the body requires knowing its SHA1 up front, which is not
			// possible when streaming.
			headers["X-Bz-Content-Sha1"] = "do_not_verify";

			if (!controlSocket_.Submit(rr_, upload_url_, "POST", std::string(), headers, this)) {
				return FZ_REPLY_ERROR;
			}
			rr_->request_.update_content_length_from_body();
			return FZ_REPLY_WOULDBLOCK;
		}
		default:
			break;
		}

		return FZ_REPLY_INTERNALERROR;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnB2Response(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			std::string code, message;
			if (rr_ && b2::parse_error(std::string(rr_->response_.body_.to_view()), code, message)) {
				log(logmsg::error, _("Server error: %s (%s)"), to_wstring(code), to_wstring(message));
			}
			else {
				log(logmsg::error, _("File transfer failed"));
			}
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (download()) {
			log(logmsg::status, _("File transfer successful"));
			controlSocket_.ResetOperation(FZ_REPLY_OK);
			return;
		}

		if (opState <= 1) {
			// Either we just resolved the bucket id or received an upload url
			if (opState == 1) {
				auto const doc = fz::json::parse(std::string(rr_->response_.body_.to_view()));
				upload_url_ = doc["uploadUrl"].string_value();
				upload_token_ = doc["authorizationToken"].string_value();
				if (upload_url_.empty() || upload_token_.empty()) {
					log(logmsg::error, _("Could not parse the server's response."));
					controlSocket_.ResetOperation(FZ_REPLY_ERROR);
					return;
				}
			}
			++opState;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}

		log(logmsg::status, _("File transfer successful"));
		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	fz::http::continuation OnHeader(std::shared_ptr<B2RequestResponse> const& srr)
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

	std::shared_ptr<B2RequestResponse> rr_;
	std::string bucket_;
	std::string bucket_id_;
	std::string object_name_;
	std::string upload_url_;
	std::string upload_token_;
};

// ---------------------------------------------------------------------------
// CB2DeleteOpData
// ---------------------------------------------------------------------------

class CB2DeleteOpData final : public COpData, public CB2OpData, public CB2RequestHandler
{
public:
	CB2DeleteOpData(CB2ControlSocket & controlSocket, CServerPath const& path, std::vector<std::wstring> files)
		: COpData(Command::del, L"CB2DeleteOpData")
		, CB2OpData(controlSocket)
		, path_(path)
		, files_(std::move(files))
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<B2RequestResponse>();

		auto split = CB2ControlSocket::SplitPath(path_);
		bucket_ = split.first;
		prefix_ = split.second;

		if (bucket_id_.empty()) {
			auto it = controlSocket_.bucket_ids_.find(bucket_);
			if (it == controlSocket_.bucket_ids_.end()) {
				if (!controlSocket_.ResolveBucket(rr_, bucket_, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			bucket_id_ = it->second;
		}

		if (files_.empty()) {
			return FZ_REPLY_OK;
		}

		if (step_ == 0) {
			current_ = files_.back();
			key_ = b2::join_name(prefix_, fz::to_utf8(current_));

			// Find the file id of the current version
			std::map<std::string, std::string> headers;
			headers["Content-Type"] = "application/json";
			if (!controlSocket_.Submit(rr_, controlSocket_.ApiUrl() + "/b2api/v3/b2_list_file_names",
					"POST", b2::make_list_request(bucket_id_, key_, std::string(), 1, key_), headers, this))
			{
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/json";
		if (!controlSocket_.Submit(rr_, controlSocket_.ApiUrl() + "/b2api/v3/b2_delete_file_version",
				"POST", b2::make_delete_file_request(key_, file_id_), headers, this))
		{
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnB2Response(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Deleting file \"%s\" failed"), current_);
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		std::string const body(rr_->response_.body_.to_view());

		if (step_ == 0) {
			b2::list_result result;
			std::string error;
			if (!b2::parse_list_file_names(body, result, error) || result.files.empty()) {
				log(logmsg::error, _("File \"%s\" not found"), current_);
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			file_id_ = result.files.front().file_id;
			step_ = 1;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}

		files_.pop_back();
		step_ = 0;
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
	std::string bucket_id_;
	std::string prefix_;
	std::string key_;
	std::string file_id_;
	int step_{};
	std::shared_ptr<B2RequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CB2MkdirOpData (placeholder object with a trailing slash)
// ---------------------------------------------------------------------------

class CB2MkdirOpData final : public COpData, public CB2OpData, public CB2RequestHandler
{
public:
	CB2MkdirOpData(CB2ControlSocket & controlSocket, CServerPath const& path)
		: COpData(Command::mkdir, L"CB2MkdirOpData")
		, CB2OpData(controlSocket)
		, path_(path)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<B2RequestResponse>();

		auto split = CB2ControlSocket::SplitPath(path_);
		bucket_ = split.first;
		name_ = ensure_trailing_slash(split.second);
		if (name_.empty()) {
			log(logmsg::error, _("Invalid path"));
			return FZ_REPLY_ERROR;
		}

		if (bucket_id_.empty()) {
			auto it = controlSocket_.bucket_ids_.find(bucket_);
			if (it == controlSocket_.bucket_ids_.end()) {
				if (!controlSocket_.ResolveBucket(rr_, bucket_, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			bucket_id_ = it->second;
		}

		if (opState == 0) {
			std::map<std::string, std::string> headers;
			headers["Content-Type"] = "application/json";
			if (!controlSocket_.Submit(rr_, controlSocket_.ApiUrl() + "/b2api/v3/b2_get_upload_url",
					"POST", b2::make_upload_url_request(bucket_id_), headers, this))
			{
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		}

		std::map<std::string, std::string> headers;
		headers["Authorization"] = upload_token_;
		headers["X-Bz-File-Name"] = b2::encode_file_name(name_);
		headers["Content-Type"] = "application/x-bz-folder";
		headers["X-Bz-Content-Sha1"] = empty_sha1_marker();
		if (!controlSocket_.Submit(rr_, upload_url_, "POST", std::string(), headers, this)) {
			return FZ_REPLY_ERROR;
		}
		return FZ_REPLY_WOULDBLOCK;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnB2Response(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Creating directory failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (opState == 0) {
			auto const doc = fz::json::parse(std::string(rr_->response_.body_.to_view()));
			upload_url_ = doc["uploadUrl"].string_value();
			upload_token_ = doc["authorizationToken"].string_value();
			if (upload_url_.empty() || upload_token_.empty()) {
				log(logmsg::error, _("Could not parse the server's response."));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			opState = 1;
			rr_.reset();
			controlSocket_.SendNextCommand();
			return;
		}

		controlSocket_.ResetOperation(FZ_REPLY_OK);
	}

private:
	static std::string empty_sha1_marker()
	{
		// Verifying the (empty) body would need its SHA1; let the server skip it.
		return "do_not_verify";
	}

	CServerPath path_;
	std::string bucket_;
	std::string bucket_id_;
	std::string name_;
	std::string upload_url_;
	std::string upload_token_;
	std::shared_ptr<B2RequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CB2RenameOpData (copy + delete)
// ---------------------------------------------------------------------------

class CB2RenameOpData final : public COpData, public CB2OpData, public CB2RequestHandler
{
public:
	CB2RenameOpData(CB2ControlSocket & controlSocket, CRenameCommand const& command)
		: COpData(Command::rename, L"CB2RenameOpData")
		, CB2OpData(controlSocket)
		, command_(command)
	{}

	virtual int Send() override
	{
		rr_ = std::make_shared<B2RequestResponse>();

		auto from = CB2ControlSocket::SplitPath(command_.GetFromPath());
		auto to = CB2ControlSocket::SplitPath(command_.GetToPath());
		bucket_ = from.first;
		source_ = b2::join_name(from.second, fz::to_utf8(command_.GetFromFile()));
		target_ = b2::join_name(to.second, fz::to_utf8(command_.GetToFile()));

		if (bucket_id_.empty()) {
			auto it = controlSocket_.bucket_ids_.find(bucket_);
			if (it == controlSocket_.bucket_ids_.end()) {
				if (!controlSocket_.ResolveBucket(rr_, bucket_, this)) {
					return FZ_REPLY_ERROR;
				}
				return FZ_REPLY_WOULDBLOCK;
			}
			bucket_id_ = it->second;
		}

		std::map<std::string, std::string> headers;
		headers["Content-Type"] = "application/json";

		switch (step_) {
		case 0: // find source file id
			if (!controlSocket_.Submit(rr_, controlSocket_.ApiUrl() + "/b2api/v3/b2_list_file_names",
					"POST", b2::make_list_request(bucket_id_, source_, std::string(), 1, source_), headers, this))
			{
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		case 1: // copy
			if (!controlSocket_.Submit(rr_, controlSocket_.ApiUrl() + "/b2api/v3/b2_copy_file",
					"POST", b2::make_copy_request(source_id_, target_, bucket_id_), headers, this))
			{
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		case 2: // delete source version
			if (!controlSocket_.Submit(rr_, controlSocket_.ApiUrl() + "/b2api/v3/b2_delete_file_version",
					"POST", b2::make_delete_file_request(source_, source_id_), headers, this))
			{
				return FZ_REPLY_ERROR;
			}
			return FZ_REPLY_WOULDBLOCK;
		default:
			break;
		}

		return FZ_REPLY_INTERNALERROR;
	}

	virtual int ParseResponse() override { return FZ_REPLY_INTERNALERROR; }

	virtual void OnB2Response(bool success) override
	{
		if (!rr_ || !success || !rr_->response_.success()) {
			log(logmsg::error, _("Renaming file failed"));
			controlSocket_.ResetOperation(FZ_REPLY_ERROR);
			return;
		}

		if (step_ == 0) {
			b2::list_result result;
			std::string error;
			if (!b2::parse_list_file_names(std::string(rr_->response_.body_.to_view()), result, error) || result.files.empty()) {
				log(logmsg::error, _("File not found"));
				controlSocket_.ResetOperation(FZ_REPLY_ERROR);
				return;
			}
			source_id_ = result.files.front().file_id;
		}

		++step_;
		rr_.reset();
		if (step_ > 2) {
			controlSocket_.ResetOperation(FZ_REPLY_OK);
			return;
		}
		controlSocket_.SendNextCommand();
	}

private:
	CRenameCommand command_;
	std::string bucket_;
	std::string bucket_id_;
	std::string source_;
	std::string target_;
	std::string source_id_;
	int step_{};
	std::shared_ptr<B2RequestResponse> rr_;
};

// ---------------------------------------------------------------------------
// CB2ControlSocket
// ---------------------------------------------------------------------------

CB2ControlSocket::CB2ControlSocket(CFileZillaEnginePrivate & engine)
	: CRealControlSocket(engine)
{
	client_.emplace(*this);
}

CB2ControlSocket::~CB2ControlSocket()
{
	client_.reset();
}

void CB2ControlSocket::ResetSocket()
{
	tls_layer_.reset();
	CRealControlSocket::ResetSocket();
}

void CB2ControlSocket::Connect(CServer const& server, Credentials const& credentials)
{
	currentServer_ = server;
	credentials_ = credentials;

	Push(std::make_unique<CB2ConnectOpData>(*this));
}

std::pair<std::string, std::string> CB2ControlSocket::SplitPath(CServerPath const& path)
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
	return {rest.substr(0, pos), ensure_trailing_slash(rest.substr(pos + 1))};
}

bool CB2ControlSocket::ResolveBucket(std::shared_ptr<B2RequestResponse> const& rr, std::string const& bucket_name,
	CB2RequestHandler * handler)
{
	std::map<std::string, std::string> headers;
	headers["Content-Type"] = "application/json";

	fz::json doc;
	doc["accountId"] = account_id_;

	// An empty name asks for all buckets, which also refreshes the cache.
	return Submit(rr, api_url_ + "/b2api/v3/b2_list_buckets", "POST", doc.to_string(), headers, handler);
}

bool CB2ControlSocket::Submit(std::shared_ptr<B2RequestResponse> const& rr, std::string const& url,
	std::string const& verb, std::string const& body, std::map<std::string, std::string> extra_headers,
	CB2RequestHandler * handler)
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

	if (!auth_token_.empty() && !extra_headers.count("Authorization")) {
		req.headers_["Authorization"] = auth_token_;
	}
	for (auto const& h : extra_headers) {
		req.headers_[h.first] = h.second;
	}

	if (!body.empty() && !req.body_) {
		req.body_ = fz::string_reader_factory(L"json", body).open(*buffer_pool_);
		if (!req.body_) {
			return false;
		}
		req.update_content_length_from_body();
	}
	else if (body.empty() && !req.body_ && (verb == "POST" || verb == "PUT")) {
		// Zero length body needs an explicit length for some servers
		req.headers_["Content-Length"] = "0";
	}

	pending_handler_ = handler;
	if (!client_->add_request(rr)) {
		pending_handler_ = nullptr;
		return false;
	}
	return true;
}

void CB2ControlSocket::OnRequestDone(uint64_t, bool success)
{
	auto * handler = pending_handler_;
	pending_handler_ = nullptr;
	if (handler) {
		handler->OnB2Response(success);
	}
}

void CB2ControlSocket::OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info)
{
	if (!tls_layer_ || source != tls_layer_.get()) {
		return;
	}
	SendAsyncRequest(std::make_unique<CCertificateNotification>(std::move(info)));
}

void CB2ControlSocket::operator()(fz::event_base const& ev)
{
	if (fz::dispatch<fz::certificate_verification_event, fz::http::client::done_event>(ev, this,
			&CB2ControlSocket::OnVerifyCert, &CB2ControlSocket::OnRequestDone))
	{
		return;
	}
	CRealControlSocket::operator()(ev);
}

void CB2ControlSocket::List(CServerPath const& path, int)
{
	Push(std::make_unique<CB2ListOpData>(*this, path));
}

void CB2ControlSocket::FileTransfer(CFileTransferCommand const& cmd)
{
	Push(std::make_unique<CB2FileTransferOpData>(*this, cmd));
}

void CB2ControlSocket::Delete(CServerPath const& path, std::vector<std::wstring>&& files)
{
	Push(std::make_unique<CB2DeleteOpData>(*this, path, std::move(files)));
}

void CB2ControlSocket::RemoveDir(CServerPath const&, std::wstring const&)
{
	log(logmsg::error, _("Removing directories is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

void CB2ControlSocket::Mkdir(CServerPath const& path, transfer_flags const&)
{
	Push(std::make_unique<CB2MkdirOpData>(*this, path));
}

void CB2ControlSocket::Rename(CRenameCommand const& command)
{
	Push(std::make_unique<CB2RenameOpData>(*this, command));
}

void CB2ControlSocket::Chmod(CChmodCommand const&)
{
	log(logmsg::status, _("Setting permissions is not supported by this protocol."));
	ResetOperation(FZ_REPLY_NOTSUPPORTED);
}

int CB2ControlSocket::DoClose(int nErrorCode)
{
	pending_handler_ = nullptr;
	if (client_) {
		client_->stop(true);
	}
	tls_layer_.reset();

	return CRealControlSocket::DoClose(nErrorCode);
}

bool CB2ControlSocket::SetAsyncRequestReply(CAsyncRequestNotification * pNotification)
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
