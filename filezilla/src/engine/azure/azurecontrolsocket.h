#ifndef FILEZILLA_ENGINE_AZURE_CONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_AZURE_CONTROLSOCKET_HEADER

#include "../controlsocket.h"

#include "../cloudendpoint.h"

#include "azureblob.h"

#include <libfilezilla/http/client.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fz {
class tls_layer;
class tls_session_info;
}

class CAzureBlobControlSocket;
class CAzureConnectOpData;
class CAzureListOpData;
class CAzureFileTransferOpData;
class CAzureDeleteOpData;
class CAzureMkdirOpData;
class CAzureRenameOpData;

typedef fz::http::client::request AzureRequest;
typedef fz::http::client::response AzureResponse;
typedef fz::http::client::request_response_holder<AzureRequest, AzureResponse> AzureRequestResponse;

class CAzureRequestHandler
{
public:
	virtual ~CAzureRequestHandler() = default;
	virtual void OnAzureResponse(bool success) = 0;
};

class azure_http_client final : public fz::http::client::client
{
public:
	explicit azure_http_client(CAzureBlobControlSocket & owner);
	~azure_http_client();

	virtual fz::socket_interface* create_socket(fz::native_string const& host, unsigned short port, bool tls) override;
	virtual void destroy_socket() override;

protected:
	virtual void on_alive() override;

private:
	CAzureBlobControlSocket & owner_;
};

/**
 * \brief Azure Blob Storage.
 *
 * Containers are the first path segment, blobs the remaining path. Authentication
 * uses SharedKey with the storage account name as user and the base64 account key
 * as password.
 */
class CAzureBlobControlSocket final : public CRealControlSocket
{
public:
	explicit CAzureBlobControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~CAzureBlobControlSocket();

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

	friend class azure_http_client;
	friend class CProtocolOpData<CAzureBlobControlSocket>;
	friend class CAzureConnectOpData;
	friend class CAzureListOpData;
	friend class CAzureFileTransferOpData;
	friend class CAzureDeleteOpData;
	friend class CAzureMkdirOpData;
	friend class CAzureRenameOpData;

	/// Fills a request: builds the URI, adds the mandatory x-ms-* headers and signs it.
	bool Submit(std::shared_ptr<AzureRequestResponse> const& rr, std::string const& verb,
		std::string const& path, std::vector<std::pair<std::string, std::string>> query,
		std::map<std::string, std::string> extra_headers, CAzureRequestHandler * handler);

	static std::pair<std::string, std::string> SplitPath(CServerPath const& path);

	void OnRequestDone(uint64_t id, bool success);
	void OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info);

	std::string host_;
	std::string account_;
	azure::credentials cred_;

	cloud::endpoint endpoint_;
	CAzureRequestHandler * pending_handler_{};
	std::unique_ptr<fz::tls_layer> tls_layer_;
	std::optional<azure_http_client> client_;
};

typedef CProtocolOpData<CAzureBlobControlSocket> CAzureOpData;

#endif
