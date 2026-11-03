#ifndef FILEZILLA_ENGINE_OBS_OBSCONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_OBS_OBSCONTROLSOCKET_HEADER

#include "../controlsocket.h"

#include "obsrequest.h"

#include <libfilezilla/http/client.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fz {
class tls_layer;
class tls_session_info;
}

class CObsControlSocket;
class CObsConnectOpData;
class CObsListOpData;
class CObsFileTransferOpData;
class CObsDeleteOpData;
class CObsMkdirOpData;
class CObsRenameOpData;

typedef fz::http::client::request ObsRequest;
typedef fz::http::client::response ObsResponse;
typedef fz::http::client::request_response_holder<ObsRequest, ObsResponse> ObsRequestResponse;

class CObsRequestHandler
{
public:
	virtual ~CObsRequestHandler() = default;
	virtual void OnObsResponse(bool success) = 0;
};

class obs_http_client final : public fz::http::client::client
{
public:
	explicit obs_http_client(CObsControlSocket & owner);
	~obs_http_client();

	virtual fz::socket_interface* create_socket(fz::native_string const& host, unsigned short port, bool tls) override;
	virtual void destroy_socket() override;

protected:
	virtual void on_alive() override;

private:
	CObsControlSocket & owner_;
};

/**
 * \brief Huawei Cloud OBS (Object Storage Service).
 *
 * Path-style requests against the regional endpoint configured as the site's
 * host (e.g. obs.cn-north-4.myhuaweicloud.com), authenticated with the OBS
 * header signature: "OBS <AccessKeyId>:<base64(HMAC-SHA1)>".
 */
class CObsControlSocket final : public CRealControlSocket
{
public:
	explicit CObsControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~CObsControlSocket();

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

	friend class obs_http_client;
	friend class CProtocolOpData<CObsControlSocket>;
	friend class CObsConnectOpData;
	friend class CObsListOpData;
	friend class CObsFileTransferOpData;
	friend class CObsDeleteOpData;
	friend class CObsMkdirOpData;
	friend class CObsRenameOpData;

	/// Fills a request: URI, mandatory headers and the OBS signature.
	/// \param query        URL parameters (not signed)
	/// \param subresources OBS sub-resources (signed, also put into the URL)
	bool Submit(std::shared_ptr<ObsRequestResponse> const& rr, std::string const& verb,
		std::string const& bucket, std::string const& key,
		std::vector<std::pair<std::string, std::string>> query,
		std::vector<std::pair<std::string, std::string>> subresources,
		std::map<std::string, std::string> extra_headers, CObsRequestHandler * handler);

	static std::pair<std::string, std::string> SplitPath(CServerPath const& path);

	void OnRequestDone(uint64_t id, bool success);
	void OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info);

	std::string host_;
	std::string bucket_hint_;
	obs::credentials cred_;

	CObsRequestHandler * pending_handler_{};
	std::unique_ptr<fz::tls_layer> tls_layer_;
	std::optional<obs_http_client> client_;
};

typedef CProtocolOpData<CObsControlSocket> CObsOpData;

#endif
