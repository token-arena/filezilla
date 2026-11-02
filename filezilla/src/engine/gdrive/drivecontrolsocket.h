#ifndef FILEZILLA_ENGINE_GDRIVE_DRIVECONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_GDRIVE_DRIVECONTROLSOCKET_HEADER

#include "../controlsocket.h"

#include "drivejson.h"

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

class CGDriveControlSocket;
class CDriveConnectOpData;
class CDriveListOpData;
class CDriveFileTransferOpData;
class CDriveDeleteOpData;
class CDriveMkdirOpData;
class CDriveRenameOpData;

typedef fz::http::client::request DriveRequest;
typedef fz::http::client::response DriveResponse;
typedef fz::http::client::request_response_holder<DriveRequest, DriveResponse> DriveRequestResponse;

class CDriveRequestHandler
{
public:
	virtual ~CDriveRequestHandler() = default;
	virtual void OnDriveResponse(bool success) = 0;
};

class drive_http_client final : public fz::http::client::client
{
public:
	explicit drive_http_client(CGDriveControlSocket & owner);
	~drive_http_client();

	virtual fz::socket_interface* create_socket(fz::native_string const& host, unsigned short port, bool tls) override;
	virtual void destroy_socket() override;

protected:
	virtual void on_alive() override;

private:
	CGDriveControlSocket & owner_;
};

/**
 * \brief Google Drive via the API v3.
 *
 * Drive addresses files by id, so paths are resolved segment by segment using
 * "name = '...' and '<parent>' in parents" queries, caching the results.
 * The OAuth2 access token is supplied as the password.
 */
class CGDriveControlSocket final : public CRealControlSocket
{
public:
	explicit CGDriveControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~CGDriveControlSocket();

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

	friend class drive_http_client;
	friend class CProtocolOpData<CGDriveControlSocket>;
	friend class CDriveConnectOpData;
	friend class CDriveListOpData;
	friend class CDriveFileTransferOpData;
	friend class CDriveDeleteOpData;
	friend class CDriveMkdirOpData;
	friend class CDriveRenameOpData;

	bool Submit(std::shared_ptr<DriveRequestResponse> const& rr, std::string const& url, std::string const& verb,
		std::map<std::string, std::string> extra_headers, std::string const& body,
		CDriveRequestHandler * handler);

	/// Splits a server path into its individual folder names ("/" -> empty list).
	static std::vector<std::string> SplitPath(CServerPath const& path);

	/// Id of the given folder path, using the cache. Empty if unknown.
	std::string CachedId(std::string const& path) const;
	void CacheId(std::string const& path, std::string const& id);

	void OnRequestDone(uint64_t id, bool success);
	void OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info);

	std::string token_;
	std::map<std::string, std::string> id_cache_; // path -> folder id

	CDriveRequestHandler * pending_handler_{};
	std::unique_ptr<fz::tls_layer> tls_layer_;
	std::optional<drive_http_client> client_;
};

typedef CProtocolOpData<CGDriveControlSocket> CDriveOpData;

#endif
