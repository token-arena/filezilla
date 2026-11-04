#ifndef FILEZILLA_ENGINE_OSS_OSSCONTROLSOCKET_HEADER
#define FILEZILLA_ENGINE_OSS_OSSCONTROLSOCKET_HEADER

#include "../controlsocket.h"

#include "../cloudendpoint.h"

#include "ossrequest.h"

#include <libfilezilla/http/client.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fz {
class tls_layer;
class tls_session_info;
}

class COssControlSocket;
class COssConnectOpData;
class COssListOpData;
class COssFileTransferOpData;
class COssDeleteOpData;
class COssMkdirOpData;
class COssRenameOpData;

typedef fz::http::client::request OssRequest;
typedef fz::http::client::response OssResponse;
typedef fz::http::client::request_response_holder<OssRequest, OssResponse> OssRequestResponse;

class COssRequestHandler
{
public:
	virtual ~COssRequestHandler() = default;
	virtual void OnOssResponse(bool success) = 0;
};

class oss_http_client final : public fz::http::client::client
{
public:
	explicit oss_http_client(COssControlSocket & owner);
	~oss_http_client();

	virtual fz::socket_interface* create_socket(fz::native_string const& host, unsigned short port, bool tls) override;
	virtual void destroy_socket() override;

protected:
	virtual void on_alive() override;

private:
	COssControlSocket & owner_;
};

/**
 * \brief Alibaba Cloud OSS (Object Storage Service).
 *
 * Path-style requests against the regional endpoint configured as the site's
 * host (e.g. oss-cn-hangzhou.aliyuncs.com), authenticated with OSS signature
 * version 1: "OSS <AccessKeyId>:<base64(HMAC-SHA1)>".
 */
class COssControlSocket final : public CRealControlSocket
{
public:
	explicit COssControlSocket(CFileZillaEnginePrivate & engine);
	virtual ~COssControlSocket();

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

	friend class oss_http_client;
	friend class CProtocolOpData<COssControlSocket>;
	friend class COssConnectOpData;
	friend class COssListOpData;
	friend class COssFileTransferOpData;
	friend class COssDeleteOpData;
	friend class COssMkdirOpData;
	friend class COssRenameOpData;

	/// Fills a request: URI, mandatory headers and the OSS signature.
	/// \param query        URL parameters (not signed)
	/// \param subresources OSS sub-resources (signed, also put into the URL)
	bool Submit(std::shared_ptr<OssRequestResponse> const& rr, std::string const& verb,
		std::string const& bucket, std::string const& key,
		std::vector<std::pair<std::string, std::string>> query,
		std::vector<std::pair<std::string, std::string>> subresources,
		std::map<std::string, std::string> extra_headers, COssRequestHandler * handler);

	/// Everything needed to rebuild a request, kept so the auto signature
	/// fallback can re-send the very same request with the other version.
	struct request_recipe {
		std::string verb;
		std::string bucket;
		std::string key;
		std::vector<std::pair<std::string, std::string>> query;
		std::vector<std::pair<std::string, std::string>> subresources;
		std::map<std::string, std::string> headers;
		bool has_body{};
	};

	bool FillRequest(std::shared_ptr<OssRequestResponse> const& rr, request_recipe const& recipe,
		oss::signature_version version, COssRequestHandler * handler);

	std::optional<request_recipe> pending_recipe_;
	std::shared_ptr<OssRequestResponse> pending_rr_;

	static std::pair<std::string, std::string> SplitPath(CServerPath const& path);

	void OnRequestDone(uint64_t id, bool success);
	void OnVerifyCert(fz::tls_layer* source, fz::tls_session_info& info);

	std::string host_;
	std::string bucket_hint_;
	std::string region_;
	oss::credentials cred_;
	oss::signature_version version_{oss::signature_version::v4};
	oss::signature_version effective_version_{oss::signature_version::v4};
	bool fallback_done_{};

	cloud::endpoint endpoint_;
	COssRequestHandler * pending_handler_{};
	std::unique_ptr<fz::tls_layer> tls_layer_;
	std::optional<oss_http_client> client_;
};

typedef CProtocolOpData<COssControlSocket> COssOpData;

#endif
