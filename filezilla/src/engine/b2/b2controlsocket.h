#ifndef FILEZILLA_ENGINE_B2_CONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_B2_CONTROLSOCKET_HEADER

#include "../controlsocket.h"

#include "../cloudendpoint.h"

#include "b2json.h"

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

class CB2ControlSocket;
class CB2ConnectOpData;
class CB2ListOpData;
class CB2FileTransferOpData;
class CB2DeleteOpData;
class CB2MkdirOpData;
class CB2RenameOpData;

typedef fz::http::client::request B2Request;
typedef fz::http::client::response B2Response;
typedef fz::http::client::request_response_holder<B2Request, B2Response> B2RequestResponse;

class CB2RequestHandler
{
public:
	virtual ~CB2RequestHandler() = default;
	virtual void OnB2Response(bool success) = 0;
};

class b2_http_client final : public fz::http::client::client
{
public:
	explicit b2_http_client(CB2ControlSocket & owner);
	~b2_http_client();

	virtual fz::socket_interface* create_socket(fz::native_string const& host, unsigned short port, bool tls) override;
	virtual void destroy_socket() override;

protected:
	virtual void on_alive() override;

private:
	CB2ControlSocket & owner_;
};

/**
 * \brief Backblaze B2 cloud storage using the native API (b2api/v3).
 *
 * Buckets are the first path segment, object keys are the remaining path.
 * Authentication uses the application key id as user name and the application
 * key as password (LogonType::normal / ask in the site manager).
 */
class CB2ControlSocket final : public CRealControlSocket
{
public:
	explicit CB2ControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~CB2ControlSocket();

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

	friend class b2_http_client;
	friend class CProtocolOpData<CB2ControlSocket>;
	friend class CB2ConnectOpData;
	friend class CB2ListOpData;
	friend class CB2FileTransferOpData;
	friend class CB2DeleteOpData;
	friend class CB2MkdirOpData;
	friend class CB2RenameOpData;

	/// Submits a request against an absolute URL (the storage API may live on a
	/// different host than the download API).
	bool Submit(std::shared_ptr<B2RequestResponse> const& rr, std::string const& url, std::string const& verb,
		std::string const& body, std::map<std::string, std::string> extra_headers, CB2RequestHandler * handler);

	/// Returns the bucket id for the given name, using the API when needed.
	bool ResolveBucket(std::shared_ptr<B2RequestResponse> const& rr, std::string const& bucket_name,
		CB2RequestHandler * handler);

	void OnRequestDone(uint64_t id, bool success);
	void OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info);

	static std::pair<std::string, std::string> SplitPath(CServerPath const& path);

	std::string const& ApiUrl() const { return api_url_; }
	std::string const& AuthToken() const { return auth_token_; }

	std::string api_url_;
	std::string download_url_;
	std::string auth_token_;
	std::string account_id_;
	std::map<std::string, std::string> bucket_ids_; // bucket name -> id

	cloud::endpoint endpoint_;
	std::string authorize_url_;
	CB2RequestHandler * pending_handler_{};
	std::unique_ptr<fz::tls_layer> tls_layer_;
	std::optional<b2_http_client> client_;
};

typedef CProtocolOpData<CB2ControlSocket> CB2OpData;

#endif
