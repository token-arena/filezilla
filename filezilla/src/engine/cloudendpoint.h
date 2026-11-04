#ifndef FILEZILLA_ENGINE_CLOUDENDPOINT_HEADER
#define FILEZILLA_ENGINE_CLOUDENDPOINT_HEADER

#include <string>

// Shared endpoint handling for the cloud protocols: the service endpoint may be
// plain HTTP as well as HTTPS, which matters for private/on-premise deployments
// (MinIO, Ceph RGW, Azure Stack, private OSS/COS/OBS installations, local
// emulators such as LocalStack, ...).
//
// No engine dependencies, so it can be unit-tested standalone
// (see tests/endpointselftest.cpp).
namespace cloud {

	struct endpoint {
		std::string scheme{"https"};
		std::string host;
		unsigned short port{443};

		bool tls() const { return scheme == "https"; }

		/// "host" or "host:port" when the port is not the default for the scheme;
		/// this is the value that must be sent in the Host header.
		std::string authority() const;

		/// "scheme://host[:port]"
		std::string url() const;

		/// Port to put into the URI, 0 when it is the scheme default.
		unsigned short uri_port() const;
	};

	/// Parses a boolean-ish parameter ("1", "true", "yes", "on" and their
	/// negatives). Returns false when the value is not recognised.
	bool parse_flag(std::string const& value, bool & out);

	/// Resolves the endpoint from the host configured for the site.
	///
	/// \param configured  "host", "host:port" or a full "scheme://host[:port]".
	///                    The port may also be omitted, then \p default_port is used.
	/// \param tls_param   Optional override for the scheme ("1"/"0"/...).
	/// \param default_port Used when no port was given; it also decides the
	///                    scheme when neither a URL scheme nor \p tls_param is
	///                    present: 443 and 8443 mean HTTPS, anything else means
	///                    plain HTTP.
	endpoint resolve(std::string const& configured, std::string const& tls_param, unsigned short default_port);

	/// Replaces the host of \p base while keeping its scheme and port, for
	/// protocols whose API host is fixed but which have to be reachable through
	/// a private gateway. Returns \p base when \p host is empty.
	endpoint with_host(endpoint const& base, std::string const& host);

	/// Replaces the 'scheme://host[:port]' prefix of \p url with the one of
	/// \p ep, keeping path and query. Used by the protocols whose API host is
	/// hard coded so they can be pointed at a private gateway.
	std::string rebase(std::string const& url, endpoint const& ep);
}

#endif
