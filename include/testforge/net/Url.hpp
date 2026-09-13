#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace testforge::net {

/// A parsed absolute URL.
struct Url {
    std::string scheme;  ///< lowercase: "http" or "https"
    std::string host;
    int port = 0;  ///< resolved from the scheme when not explicit
    std::string path = "/";
    std::string query;  ///< without the leading '?'
    std::string fragment;

    [[nodiscard]] bool isHttps() const { return scheme == "https"; }

    /// Path plus query, i.e. the request target sent on the wire.
    [[nodiscard]] std::string requestTarget() const;

    /// "host:port", with the port omitted when it is the scheme default.
    [[nodiscard]] std::string authority() const;

    [[nodiscard]] std::string toString() const;
};

/// Parses an absolute URL. Returns nullopt for anything that is not a
/// well-formed http/https URL — including relative paths, which callers must
/// resolve against a base first.
std::optional<Url> parseUrl(std::string_view text);

/// Joins a base URL and a path: joinUrl("http://h:8000/api", "/users/1").
///
/// Path traversal is rejected rather than normalised: a path containing ".."
/// returns nullopt. Test specifications come from configuration files and from
/// a language model, and silently resolving "../.." into a different endpoint
/// would turn a typo into a request nobody intended.
std::optional<std::string> joinUrl(std::string_view base, std::string_view path);

/// Percent-encodes a string for use in a query value.
std::string urlEncode(std::string_view text);

std::string urlDecode(std::string_view text);

/// Parses "a=1&b=2" into pairs, decoding both sides.
std::vector<std::pair<std::string, std::string>> parseQuery(std::string_view query);

/// Rules about which URLs may be requested.
///
/// This is TestForge's answer to server-side request forgery. A test
/// specification produced by a language model is untrusted input; without a
/// policy, "generate tests for my API" could produce a request to
/// http://169.254.169.254/ (the cloud metadata endpoint) and TestForge would
/// dutifully make it. See docs/security.md.
struct UrlPolicy {
    /// Only these schemes are ever allowed.
    std::vector<std::string> allowedSchemes = {"http", "https"};

    /// When non-empty, the host must match one of these exactly
    /// (case-insensitive). Empty means "any host that passes the other rules".
    std::vector<std::string> allowedHosts;

    /// Blocks link-local and cloud metadata addresses.
    bool blockLinkLocal = true;

    /// Blocks loopback. Off by default: the whole point of the sample service
    /// is that it runs on localhost.
    bool blockLoopback = false;

    /// Blocks RFC 1918 private ranges. Off by default for the same reason.
    bool blockPrivateNetworks = false;

    /// Returns an empty string when `url` is acceptable, or the reason it is
    /// not. Checks are performed on the literal host in the URL; TestForge
    /// does not re-resolve after connecting, so this is a policy check rather
    /// than a defence against DNS rebinding (documented in security.md).
    [[nodiscard]] std::string reject(const Url& url) const;

    [[nodiscard]] std::string reject(std::string_view rawUrl) const;

    /// A policy that only allows the given base URL's host.
    static UrlPolicy restrictedTo(std::string_view baseUrl);
};

}  // namespace testforge::net
