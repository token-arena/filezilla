#ifndef FILEZILLA_ENGINE_COS_COSCONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_COS_COSCONTROLSOCKET_HEADER

#include "../controlsocket.h"

#include "cossign.h"

#include <libfilezilla/http/client.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fz {
class tls_layer;
class tls_session_info;
}

class CCosControlSocket;
class CCosConnectOpData;
class CCosListOpData;
class CCosFileTransferOpData;
class CCosDeleteOpData;
class CCosMkdirOpData;
class CCosRenameOpData;

typedef fz::http::client::request CosRequest;
typedef fz::http::client::response CosResponse;
typedef fz::http::client::request_response_holder<CosRequest, CosResponse> CosRequestResponse;

class CCosRequestHandler
{
public:
	virtual ~CCosRequestHandler() = default;
	virtual void OnCosResponse(bool success) = 0;
};

class cos_http_client final : public fz::http::client::client
{
public:
	explicit cos_http_client(CCosControlSocket & owner);
	~cos_http_client();

	virtual fz::socket_interface* create_socket(fz::native_string const& host, unsigned short port, bool tls) override;
	virtual void destroy_socket() override;

protected:
	virtual void on_alive() override;

private:
	CCosControlSocket & owner_;
};

/**
 * \brief Tencent Cloud COS (Cloud Object Storage).
 *
 * COS addresses the bucket through the host name
 * (<bucket>-<appid>.cos.<region>.myqcloud.com), so the configured host is used
 * verbatim and FileZilla paths map directly onto object keys; a site therefore
 * represents exactly one bucket. Authentication uses the documented
 * q-sign-algorithm=sha1 signature.
 */
class CCosControlSocket final : public CRealControlSocket
{
public:
	explicit CCosControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~CCosControlSocket();

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

	friend class cos_http_client;
	friend class CProtocolOpData<CCosControlSocket>;
	friend class CCosConnectOpData;
	friend class CCosListOpData;
	friend class CCosFileTransferOpData;
	friend class CCosDeleteOpData;
	friend class CCosMkdirOpData;
	friend class CCosRenameOpData;

	/// Fills a request: URI, headers (Host included) and the COS signature.
	bool Submit(std::shared_ptr<CosRequestResponse> const& rr, std::string const& verb,
		std::string const& key_path, std::vector<std::pair<std::string, std::string>> query,
		std::map<std::string, std::string> extra_headers, CCosRequestHandler * handler);

	/// Returns the key path for a server path, without leading but with trailing
	/// slash for directories ("" for the root, "dir/" for /dir).
	static std::string KeyPrefix(CServerPath const& path);

	void OnRequestDone(uint64_t id, bool success);
	void OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info);

	std::string host_;
	tcos::credentials cred_;

	CCosRequestHandler * pending_handler_{};
	std::unique_ptr<fz::tls_layer> tls_layer_;
	std::optional<cos_http_client> client_;
};

typedef CProtocolOpData<CCosControlSocket> CCosOpData;

#endif
