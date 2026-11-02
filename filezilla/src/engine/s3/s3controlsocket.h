#ifndef FILEZILLA_ENGINE_S3_CONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_S3_CONTROLSOCKET_HEADER

#include "../controlsocket.h"

#include "s3signer.h"

#include <libfilezilla/http/client.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fz {
class tls_layer;
class tls_session_info;
}

class CS3ControlSocket;
class CS3ConnectOpData;
class CS3ListOpData;
class CS3FileTransferOpData;
class CS3DeleteOpData;
class CS3MkdirOpData;
class CS3RenameOpData;

typedef fz::http::client::request S3Request;
typedef fz::http::client::response S3Response;
typedef fz::http::client::request_response_holder<S3Request, S3Response> S3RequestResponse;

/// Small helper so op data objects can be notified when their request finished.
class CS3RequestHandler
{
public:
	virtual ~CS3RequestHandler() = default;
	virtual void OnS3Response(bool success) = 0;
};

/// HTTP client bound to a control socket, mirroring the pattern used by
/// CHttpControlSocket: it owns socket creation so the control socket keeps
/// control over TLS, proxies and rate limiting.
class s3_http_client final : public fz::http::client::client
{
public:
	explicit s3_http_client(CS3ControlSocket & owner);
	~s3_http_client();

	virtual fz::socket_interface* create_socket(fz::native_string const& host, unsigned short port, bool tls) override;
	virtual void destroy_socket() override;

protected:
	virtual void on_alive() override;

private:
	CS3ControlSocket & owner_;
};

/**
 * \brief Amazon S3 and S3-compatible object storage engine.
 *
 * Uses path-style addressing so it works against AWS S3, Cloudflare R2, MinIO,
 * Backblaze B2 (S3 API) and other compatible endpoints. Requests are signed
 * with AWS Signature Version 4 (see s3signer.h, verified in tests/s3selftest.cpp).
 */
class CS3ControlSocket final : public CRealControlSocket
{
public:
	explicit CS3ControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~CS3ControlSocket();

	virtual void Connect(CServer const& server, Credentials const& credentials) override;

protected:
	virtual void List(CServerPath const& path, int flags) override;
	virtual void FileTransfer(CFileTransferCommand const& cmd) override;
	virtual void Delete(CServerPath const& path, std::vector<std::wstring>&& files) override;
	virtual void RemoveDir(CServerPath const& path, std::wstring const& subDir) override;
	virtual void Mkdir(CServerPath const& path, transfer_flags const& flags) override;
	virtual void Rename(CRenameCommand const& command) override;
	virtual void Chmod(CChmodCommand const& command) override;

	virtual int DoClose(int nErrorCode = FZ_REPLY_DISCONNECTED | FZ_REPLY_ERROR) override;
	virtual bool SetAsyncRequestReply(CAsyncRequestNotification * pNotification) override;
	virtual void operator()(fz::event_base const& ev) override;
	virtual void ResetSocket() override;

	friend class s3_http_client;
	friend class CProtocolOpData<CS3ControlSocket>;
	friend class CS3ConnectOpData;
	friend class CS3ListOpData;
	friend class CS3FileTransferOpData;
	friend class CS3DeleteOpData;
	friend class CS3MkdirOpData;
	friend class CS3RenameOpData;

	// --- helpers used by the op data classes ---

	/// Splits a server path into bucket name and prefix ("" or "a/b/").
	static std::pair<std::string, std::string> SplitPath(CServerPath const& path);

	/// "/bucket" for the given server path (or "/" for the root).
	static std::string BucketPath(CServerPath const& path);

	/// "/bucket/prefix/filename", percent-decoding left to fz::uri.
	static std::string ObjectPath(CServerPath const& path, std::wstring const& file);

	/// "/bucket/prefix/" used as placeholder object for directories.
	static std::string FolderPath(CServerPath const& path);

	/// Fills request verb, URI, headers and SigV4 signature.
	void FillRequest(S3Request & req, std::string const& verb, std::string const& raw_path,
		std::vector<std::pair<std::string, std::string>> const& query = {},
		std::string const& payload_hash = {}, std::map<std::string, std::string> extra_headers = {});

	/// Hands a request over to the HTTP client. Returns false on failure.
	bool Submit(std::shared_ptr<S3RequestResponse> const& rr, CS3RequestHandler * handler);

	void OnRequestDone(uint64_t id, bool success);
	void OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info);

	// endpoint information, filled in by Connect()
	std::string host_;     // hostname used for connecting
	std::string host_hdr_; // Host header value (with port when non-default)
	unsigned short port_{443};
	bool tls_{true};
	std::string region_{"us-east-1"};
	std::string service_{"s3"};
	s3::credentials cred_;
	std::map<std::string, std::string> default_headers_; // e.g. server-side encryption

	CS3RequestHandler * pending_handler_{};
	std::unique_ptr<fz::tls_layer> tls_layer_;
	std::optional<s3_http_client> client_;
};

typedef CProtocolOpData<CS3ControlSocket> CS3OpData;

#endif
