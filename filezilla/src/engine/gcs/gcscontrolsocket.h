#ifndef FILEZILLA_ENGINE_GCS_CONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_GCS_CONTROLSOCKET_HEADER

#include "../controlsocket.h"

#include "../cloudendpoint.h"

#include "gcsjson.h"

#include <libfilezilla/http/client.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fz {
class tls_layer;
class tls_session_info;
}

class CGcsControlSocket;
class CGcsConnectOpData;
class CGcsListOpData;
class CGcsFileTransferOpData;
class CGcsDeleteOpData;
class CGcsMkdirOpData;
class CGcsRenameOpData;

typedef fz::http::client::request GcsRequest;
typedef fz::http::client::response GcsResponse;
typedef fz::http::client::request_response_holder<GcsRequest, GcsResponse> GcsRequestResponse;

class CGcsRequestHandler
{
public:
	virtual ~CGcsRequestHandler() = default;
	virtual void OnGcsResponse(bool success) = 0;
};

class gcs_http_client final : public fz::http::client::client
{
public:
	explicit gcs_http_client(CGcsControlSocket & owner);
	~gcs_http_client();

	virtual fz::socket_interface* create_socket(fz::native_string const& host, unsigned short port, bool tls) override;
	virtual void destroy_socket() override;

protected:
	virtual void on_alive() override;

private:
	CGcsControlSocket & owner_;
};

/**
 * \brief Google Cloud Storage via the JSON API (storage/v1).
 *
 * Buckets are the first path segment, objects the remaining path. The access
 * token (OAuth2 bearer token) is supplied as the password, an optional project
 * id for bucket listings can be given through the "project" extra parameter.
 */
class CGcsControlSocket final : public CRealControlSocket
{
public:
	explicit CGcsControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~CGcsControlSocket();

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

	friend class gcs_http_client;
	friend class CProtocolOpData<CGcsControlSocket>;
	friend class CGcsConnectOpData;
	friend class CGcsListOpData;
	friend class CGcsFileTransferOpData;
	friend class CGcsDeleteOpData;
	friend class CGcsMkdirOpData;
	friend class CGcsRenameOpData;

	bool Submit(std::shared_ptr<GcsRequestResponse> const& rr, std::string const& url, std::string const& verb,
		std::map<std::string, std::string> extra_headers, CGcsRequestHandler * handler);

	static std::pair<std::string, std::string> SplitPath(CServerPath const& path);

	void OnRequestDone(uint64_t id, bool success);
	void OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info);

	std::string token_;
	std::string project_;

	cloud::endpoint endpoint_;
	bool has_endpoint_override_{};
	CGcsRequestHandler * pending_handler_{};
	std::unique_ptr<fz::tls_layer> tls_layer_;
	std::optional<gcs_http_client> client_;
};

typedef CProtocolOpData<CGcsControlSocket> CGcsOpData;

#endif
