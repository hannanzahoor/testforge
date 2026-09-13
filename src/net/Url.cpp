#include "testforge/net/Url.hpp"

#include "testforge/core/StringUtils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace testforge::net {
namespace {

int defaultPortFor(std::string_view scheme) {
    if (scheme == "https") {
        return 443;
    }
    if (scheme == "http") {
        return 80;
    }
    return 0;
}

bool isIpv4Literal(std::string_view host, std::array<int, 4>& octets) {
    int values[4] = {-1, -1, -1, -1};
    int index = 0;
    int current = -1;
    for (const char c : host) {
        if (c >= '0' && c <= '9') {
            current = (current < 0 ? 0 : current) * 10 + (c - '0');
            if (current > 255) {
                return false;
            }
        } else if (c == '.') {
            if (current < 0 || index >= 3) {
                return false;
            }
            values[index++] = current;
            current = -1;
        } else {
            return false;
        }
    }
    if (current < 0 || index != 3) {
        return false;
    }
    values[3] = current;
    for (int i = 0; i < 4; ++i) {
        octets[static_cast<std::size_t>(i)] = values[i];
    }
    return true;
}

}  // namespace

std::string Url::requestTarget() const {
    std::string target = path.empty() ? "/" : path;
    if (!query.empty()) {
        target.push_back('?');
        target.append(query);
    }
    return target;
}

std::string Url::authority() const {
    if (port == 0 || port == defaultPortFor(scheme)) {
        return host;
    }
    return host + ":" + std::to_string(port);
}

std::string Url::toString() const {
    std::ostringstream os;
    os << scheme << "://" << authority() << requestTarget();
    if (!fragment.empty()) {
        os << '#' << fragment;
    }
    return os.str();
}

std::optional<Url> parseUrl(std::string_view text) {
    const std::string trimmed = strings::trim(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    const std::size_t schemeEnd = trimmed.find("://");
    if (schemeEnd == std::string::npos || schemeEnd == 0) {
        return std::nullopt;
    }

    Url url;
    url.scheme = strings::toLower(trimmed.substr(0, schemeEnd));
    for (const char c : url.scheme) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '+' && c != '-' && c != '.') {
            return std::nullopt;
        }
    }

    std::string rest = trimmed.substr(schemeEnd + 3);
    if (rest.empty()) {
        return std::nullopt;
    }

    // Strip the fragment first: everything after '#' belongs to no other part.
    if (const std::size_t hash = rest.find('#'); hash != std::string::npos) {
        url.fragment = rest.substr(hash + 1);
        rest = rest.substr(0, hash);
    }

    std::string authority = rest;
    std::string pathAndQuery = "/";
    if (const std::size_t slash = rest.find('/'); slash != std::string::npos) {
        authority = rest.substr(0, slash);
        pathAndQuery = rest.substr(slash);
    }

    // Userinfo is parsed and discarded: credentials in a URL are a security
    // hazard (they end up in logs) and TestForge has no use for them.
    if (const std::size_t at = authority.find('@'); at != std::string::npos) {
        authority = authority.substr(at + 1);
    }
    if (authority.empty()) {
        return std::nullopt;
    }

    // IPv6 literal: [::1]:8080
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) {
            return std::nullopt;
        }
        url.host = authority.substr(1, close - 1);
        const std::string tail = authority.substr(close + 1);
        if (!tail.empty()) {
            if (tail.front() != ':') {
                return std::nullopt;
            }
            std::int64_t port = 0;
            if (!strings::parseInt(tail.substr(1), port) || port <= 0 || port > 65535) {
                return std::nullopt;
            }
            url.port = static_cast<int>(port);
        }
    } else if (const std::size_t colon = authority.rfind(':'); colon != std::string::npos) {
        url.host = authority.substr(0, colon);
        std::int64_t port = 0;
        if (!strings::parseInt(authority.substr(colon + 1), port) || port <= 0 || port > 65535) {
            return std::nullopt;
        }
        url.port = static_cast<int>(port);
    } else {
        url.host = authority;
    }

    if (url.host.empty()) {
        return std::nullopt;
    }
    url.host = strings::toLower(url.host);
    if (url.port == 0) {
        url.port = defaultPortFor(url.scheme);
    }

    if (const std::size_t question = pathAndQuery.find('?'); question != std::string::npos) {
        url.path = pathAndQuery.substr(0, question);
        url.query = pathAndQuery.substr(question + 1);
    } else {
        url.path = pathAndQuery;
    }
    if (url.path.empty()) {
        url.path = "/";
    }

    return url;
}

std::optional<std::string> joinUrl(std::string_view base, std::string_view path) {
    if (strings::contains(path, "..")) {
        return std::nullopt;  // see the header: traversal is rejected, not fixed
    }
    // An absolute URL overrides the base entirely.
    if (strings::startsWith(path, "http://") || strings::startsWith(path, "https://")) {
        return parseUrl(path).has_value() ? std::optional<std::string>(std::string(path))
                                          : std::nullopt;
    }

    const std::optional<Url> parsedBase = parseUrl(base);
    if (!parsedBase.has_value()) {
        return std::nullopt;
    }

    std::string basePath = parsedBase->path;
    while (!basePath.empty() && basePath.back() == '/') {
        basePath.pop_back();
    }

    std::string suffix(path);
    if (suffix.empty()) {
        suffix = "/";
    } else if (suffix.front() != '/') {
        suffix.insert(suffix.begin(), '/');
    }

    Url joined = *parsedBase;
    const std::size_t question = suffix.find('?');
    if (question != std::string::npos) {
        joined.query = suffix.substr(question + 1);
        suffix = suffix.substr(0, question);
    } else {
        joined.query.clear();
    }
    joined.path = basePath + suffix;
    joined.fragment.clear();
    return joined.toString();
}

std::string urlEncode(std::string_view text) {
    static constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        if (std::isalnum(byte) != 0 || byte == '-' || byte == '_' || byte == '.' || byte == '~') {
            out.push_back(raw);
        } else {
            out.push_back('%');
            out.push_back(kHex[byte >> 4u]);
            out.push_back(kHex[byte & 0x0Fu]);
        }
    }
    return out;
}

std::string urlDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+') {
            out.push_back(' ');
            continue;
        }
        if (text[i] == '%' && i + 2 < text.size()) {
            const auto hexValue = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            const int high = hexValue(text[i + 1]);
            const int low = hexValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parseQuery(std::string_view query) {
    std::vector<std::pair<std::string, std::string>> pairs;
    for (const std::string& part : strings::split(query, '&', true)) {
        const std::size_t eq = part.find('=');
        if (eq == std::string::npos) {
            pairs.emplace_back(urlDecode(part), std::string{});
        } else {
            pairs.emplace_back(urlDecode(part.substr(0, eq)), urlDecode(part.substr(eq + 1)));
        }
    }
    return pairs;
}

// ---------------------------------------------------------------------------
// UrlPolicy
// ---------------------------------------------------------------------------

std::string UrlPolicy::reject(const Url& url) const {
    const bool schemeAllowed = std::any_of(
        allowedSchemes.begin(), allowedSchemes.end(), [&url](const std::string& scheme) {
            return scheme == url.scheme;
        });
    if (!schemeAllowed) {
        return "scheme '" + url.scheme + "' is not allowed";
    }

    if (!allowedHosts.empty()) {
        const bool hostAllowed =
            std::any_of(allowedHosts.begin(), allowedHosts.end(), [&url](const std::string& host) {
                return strings::equalsIgnoreCase(host, url.host);
            });
        if (!hostAllowed) {
            return "host '" + url.host + "' is not in the allow-list";
        }
        // An explicit allow-list is a deliberate decision by the operator, so
        // the address-range rules below do not second-guess it.
        return {};
    }

    std::array<int, 4> octets{};
    const bool isIpv4 = isIpv4Literal(url.host, octets);
    const bool isLoopbackName =
        strings::equalsIgnoreCase(url.host, "localhost") || url.host == "::1";

    if (blockLoopback && (isLoopbackName || (isIpv4 && octets[0] == 127))) {
        return "loopback addresses are blocked by policy";
    }

    if (blockLinkLocal && isIpv4 && octets[0] == 169 && octets[1] == 254) {
        // 169.254.169.254 is the cloud metadata endpoint on AWS, GCP and Azure.
        return "link-local addresses (including cloud metadata endpoints) are blocked";
    }
    if (blockLinkLocal && strings::startsWith(url.host, "fe80:")) {
        return "link-local addresses are blocked";
    }

    if (blockPrivateNetworks && isIpv4) {
        const bool privateRange = octets[0] == 10 ||
                                  (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
                                  (octets[0] == 192 && octets[1] == 168);
        if (privateRange) {
            return "private network addresses are blocked by policy";
        }
    }

    return {};
}

std::string UrlPolicy::reject(std::string_view rawUrl) const {
    const std::optional<Url> parsed = parseUrl(rawUrl);
    if (!parsed.has_value()) {
        return "not a valid absolute http(s) URL";
    }
    return reject(*parsed);
}

UrlPolicy UrlPolicy::restrictedTo(std::string_view baseUrl) {
    UrlPolicy policy;
    if (const std::optional<Url> parsed = parseUrl(baseUrl); parsed.has_value()) {
        policy.allowedHosts.push_back(parsed->host);
    }
    return policy;
}

}  // namespace testforge::net
