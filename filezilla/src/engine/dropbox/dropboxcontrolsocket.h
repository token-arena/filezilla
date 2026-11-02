#ifndef FILEZILLA_ENGINE_DROPBOX_DROPBOXCONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_DROPBOX_DROPBOXCONTROLSOCKET_HEADER

#include "../controlsocket.h"

#include "dropboxjson.h"

#include <libfilezilla/http/client.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fz {
class tls_layer;
class tls_session_info;
}

class CDropboxControlSocket;
class CDropboxConnectOpData;
class CDropboxListOpData;
class CDropboxFileTransferOpData;
class CDropboxDeleteOpData;
class CDropboxMkdirOpData;
class CDropboxRenameOpData;

typedef fz::http::client::request DropboxRequest;
typedef fz::http::client::response DropboxResponse;
typedef fz::http::client::request_response_holder<DropboxRequest, DropboxResponse> DropboxRequestResponse;

class CDropboxRequestHandler
{
public:
	virtual ~CDropboxRequestHandler() = default;
	virtual void OnDropboxResponse(bool success) = 0;
};

class dropbox_http_client final : public fz::http::client::client
{
public:
	explicit dropbox_http_client(CDropboxControlSocket & owner);
	~dropbox_http_client();

	virtual fz::socket_interface* create_socket(fz::native_string const& host, unsigned short port, bool tls) override;
	virtual void destroy_socket() override;

protected:
	virtual void on_alive() override;

private:
	CDropboxControlSocket & owner_;
};

/**
 * \brief Dropbox via the API v2.
 *
 * Dropbox paths are absolute ("/a/b.txt") which maps directly onto FileZilla's
 * server paths. The OAuth2 access token is supplied as the password.
 */
class CDropboxControlSocket final : public CRealControlSocket
{
public:
	explicit CDropboxControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~CDropboxControlSocket();

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

	friend class dropbox_http_client;
	friend class CProtocolOpData<CDropboxControlSocket>;
	friend class CDropboxConnectOpData;
	friend class CDropboxListOpData;
	friend class CDropboxFileTransferOpData;
	friend class CDropboxDeleteOpData;
	friend class CDropboxMkdirOpData;
	friend class CDropboxRenameOpData;

	bool Submit(std::shared_ptr<DropboxRequestResponse> const& rr, std::string const& url, std::string const& verb,
		std::map<std::string, std::string> extra_headers, std::string const& body, CDropboxRequestHandler * handler);

	/// Converts a server path into the "" (root) or "/a/b" form Dropbox uses.
	static std::string PathToString(CServerPath const& path);

	void OnRequestDone(uint64_t id, bool success);
	void OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info);

	std::string token_;

	CDropboxRequestHandler * pending_handler_{};
	std::unique_ptr<fz::tls_layer> tls_layer_;
	std::optional<dropbox_http_client> client_;
};

typedef CProtocolOpData<CDropboxControlSocket> CDropboxOpData;

#endif
