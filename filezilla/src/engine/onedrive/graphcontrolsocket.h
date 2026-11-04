#ifndef FILEZILLA_ENGINE_ONEDRIVE_GRAPHCONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_ONEDRIVE_GRAPHCONTROLSOCKET_HEADER

#include "../controlsocket.h"

#include "../cloudendpoint.h"

#include "graphjson.h"

#include <libfilezilla/http/client.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fz {
class tls_layer;
class tls_session_info;
}

class COneDriveControlSocket;
class COneDriveConnectOpData;
class COneDriveListOpData;
class COneDriveFileTransferOpData;
class COneDriveDeleteOpData;
class COneDriveMkdirOpData;
class COneDriveRenameOpData;

typedef fz::http::client::request GraphRequest;
typedef fz::http::client::response GraphResponse;
typedef fz::http::client::request_response_holder<GraphRequest, GraphResponse> GraphRequestResponse;

class CGraphRequestHandler
{
public:
	virtual ~CGraphRequestHandler() = default;
	virtual void OnGraphResponse(bool success) = 0;
};

class graph_http_client final : public fz::http::client::client
{
public:
	explicit graph_http_client(COneDriveControlSocket & owner);
	~graph_http_client();

	virtual fz::socket_interface* create_socket(fz::native_string const& host, unsigned short port, bool tls) override;
	virtual void destroy_socket() override;

protected:
	virtual void on_alive() override;

private:
	COneDriveControlSocket & owner_;
};

/**
 * \brief Microsoft OneDrive via the Graph API.
 *
 * FileZilla paths map directly onto Graph's path addressing ("root:/a/b:").
 * The OAuth2 access token is supplied as the password.
 */
class COneDriveControlSocket final : public CRealControlSocket
{
public:
	explicit COneDriveControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~COneDriveControlSocket();

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

	friend class graph_http_client;
	friend class CProtocolOpData<COneDriveControlSocket>;
	friend class COneDriveConnectOpData;
	friend class COneDriveListOpData;
	friend class COneDriveFileTransferOpData;
	friend class COneDriveDeleteOpData;
	friend class COneDriveMkdirOpData;
	friend class COneDriveRenameOpData;

	bool Submit(std::shared_ptr<GraphRequestResponse> const& rr, std::string const& url, std::string const& verb,
		std::map<std::string, std::string> extra_headers, std::string const& body, CGraphRequestHandler * handler);

	/// Converts a server path into the "/a/b" form used by Graph (no trailing slash).
	static std::string PathToString(CServerPath const& path);

	void OnRequestDone(uint64_t id, bool success);
	void OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info);

	std::string token_;

	cloud::endpoint endpoint_;
	bool has_endpoint_override_{};
	CGraphRequestHandler * pending_handler_{};
	std::unique_ptr<fz::tls_layer> tls_layer_;
	std::optional<graph_http_client> client_;
};

typedef CProtocolOpData<COneDriveControlSocket> COneDriveOpData;

#endif
