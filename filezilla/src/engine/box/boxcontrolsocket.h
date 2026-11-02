#ifndef FILEZILLA_ENGINE_BOX_BOXCONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_BOX_BOXCONTROLSOCKET_HEADER

#include "../controlsocket.h"

#include "boxjson.h"

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

class CBoxControlSocket;
class CBoxConnectOpData;
class CBoxListOpData;
class CBoxFileTransferOpData;
class CBoxDeleteOpData;
class CBoxMkdirOpData;
class CBoxRenameOpData;

typedef fz::http::client::request BoxRequest;
typedef fz::http::client::response BoxResponse;
typedef fz::http::client::request_response_holder<BoxRequest, BoxResponse> BoxRequestResponse;

class CBoxRequestHandler
{
public:
	virtual ~CBoxRequestHandler() = default;
	virtual void OnBoxResponse(bool success) = 0;
};

class box_http_client final : public fz::http::client::client
{
public:
	explicit box_http_client(CBoxControlSocket & owner);
	~box_http_client();

	virtual fz::socket_interface* create_socket(fz::native_string const& host, unsigned short port, bool tls) override;
	virtual void destroy_socket() override;

protected:
	virtual void on_alive() override;

private:
	CBoxControlSocket & owner_;
};

/**
 * \brief Box via the API v2.
 *
 * Box addresses folders by id, so paths are resolved segment by segment and the
 * resulting folder ids are cached. The OAuth2 access token is supplied as the
 * password.
 *
 * Uploading a *new* file requires a multipart request (or an upload session),
 * which the streaming reader abstraction cannot express yet; uploading a new
 * version of an existing file works and is used when the name already exists.
 */
class CBoxControlSocket final : public CRealControlSocket
{
public:
	explicit CBoxControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~CBoxControlSocket();

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

	friend class box_http_client;
	friend class CProtocolOpData<CBoxControlSocket>;
	friend class CBoxConnectOpData;
	friend class CBoxListOpData;
	friend class CBoxFileTransferOpData;
	friend class CBoxDeleteOpData;
	friend class CBoxMkdirOpData;
	friend class CBoxRenameOpData;

	bool Submit(std::shared_ptr<BoxRequestResponse> const& rr, std::string const& url, std::string const& verb,
		std::map<std::string, std::string> extra_headers, std::string const& body, CBoxRequestHandler * handler,
		bool with_auth = true);

	/// Splits a server path into individual folder names ("/" -> empty).
	static std::vector<std::string> SplitPath(CServerPath const& path);
	/// Converts a server path into the "/a/b" form.
	static std::string PathToString(CServerPath const& path);

	std::string CachedId(std::string const& path) const;
	void CacheId(std::string const& path, std::string const& id);

	void OnRequestDone(uint64_t id, bool success);
	void OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info);

	std::string token_;
	std::map<std::string, std::string> id_cache_; // path -> folder id

	CBoxRequestHandler * pending_handler_{};
	std::unique_ptr<fz::tls_layer> tls_layer_;
	std::optional<box_http_client> client_;
};

typedef CProtocolOpData<CBoxControlSocket> CBoxOpData;

#endif
