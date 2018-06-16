//
// Created by yuwenyong on 18-1-11.
//

#include "net4cxx/plugins/websocket/util.h"
#include "net4cxx/common/crypto/base64.h"
#include "net4cxx/common/httputils/httplib.h"
#include "net4cxx/common/utilities/random.h"


NS_BEGIN

boost::optional<Duration> Timings::diffDura(const std::string &startKey, const std::string &endKey) {
    boost::optional<Duration> d;
    auto start = _timings.find(startKey);
    auto end = _timings.find(endKey);
    if (start != _timings.end() && end != _timings.end()) {
        d = end->second - start->second;
    }
    return d;
}

boost::optional<double> Timings::diff(const std::string &startKey, const std::string &endKey) {
    auto duration = diffDura(startKey, endKey);
    boost::optional<double> d;
    if (duration) {
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(*duration);
        *duration -= seconds;
        auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(*duration);
        *duration -= milliseconds;
        auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(*duration);
        *duration -= microseconds;
        auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(*duration);
        d = 1.0 * seconds.count() + 0.001 * milliseconds.count() + 0.000001 * microseconds.count() +
            0.000000001 * nanoseconds.count();
    }
    return d;
}

std::string Timings::diffFormatted(const std::string &startKey, const std::string &endKey) {
    auto d = diff(startKey, endKey);
    std::string s;
    if (d) {
        if (*d < 0.00001) {
            // < 10us
            s = std::to_string((long long int)std::round(*d * 1000000000.)) + " ns";
        } else if (*d < 0.01) {
            // < 10ms
            s = std::to_string((long long int)std::round(*d * 1000000.)) + " us";
        } else if (*d < 10) {
            // < 10s
            s = std::to_string((long long int)std::round(*d * 1000.)) + " ms";
        } else {
            s = std::to_string((long long int)std::round(*d)) + " s";
        }
    } else {
        s = "n.a.";
    }
    StrUtil::rjustInplace(s, 8);
    return s;
}


WebSocketUtil::ParseUrlResult WebSocketUtil::parseUrl(const std::string &url) {
    auto parsed = URLParse::urlParse(url);
    const auto &scheme = parsed.getScheme();
    if (scheme != "ws" && scheme != "wss") {
        NET4CXX_THROW_EXCEPTION(Exception, "invalid WebSocket URL: protocol scheme '" + scheme +
                                           "' is not for WebSocket");
    }
    auto hostname = parsed.getHostName();
    if (!parsed.getHostName() || (*hostname).empty()) {
        NET4CXX_THROW_EXCEPTION(Exception, "invalid WebSocket URL: missing hostname");
    }
    auto port = parsed.getPort();
    if (!port) {
        port = scheme == "ws" ? HTTP_PORT : HTTPS_PORT;
    }
    const auto &fragment = parsed.getFragment();
    if (!fragment.empty()) {
        NET4CXX_THROW_EXCEPTION(Exception, "invalid WebSocket URL: non-empty fragment '" + fragment + "'");
    }
    std::string path, ppath;
    if (!parsed.getPath().empty()) {
        ppath = parsed.getPath();
        path = URLParse::unquote(ppath);
    } else {
        ppath = "/";
        path = ppath;
    }
    std::string resource;
    QueryArgListMap params;
    if (!parsed.getQuery().empty()) {
        resource = ppath + "?" + parsed.getQuery();
        params = URLParse::parseQS(parsed.getQuery());
    } else {
        resource = ppath;
    }
    return std::make_tuple(scheme == "wss", std::move(*hostname), *port, std::move(resource), std::move(path),
                           std::move(params));
}

WebSocketUtil::ParseHttpHeaderResult WebSocketUtil::parseHttpHeader(const std::string &data) {
    auto raw = StrUtil::splitLines(data);
    auto httpStatusLine = boost::trim_copy(raw[0]);
    StringMap httpHeaders;
    std::map<std::string, int> httpHeadersCnt;
    std::string::size_type j;
    std::string key, value;
    for (auto i = 1; i != raw.size(); ++i) {
        auto &h = raw[i];
        j = h.find(':');
        if (j != std::string::npos) {
            key.assign(h.begin(), std::next(h.begin(), j));
            boost::trim(key);
            boost::to_lower(key);

            value.assign(std::next(h.begin(), j + 1), h.end());
            boost::trim(value);

            if (httpHeaders.find(key) != httpHeaders.end()) {
                httpHeaders[key] += ", " + value;
                httpHeadersCnt[key] += 1;
            } else {
                httpHeaders[key] = std::move(value);
                httpHeadersCnt[key] = 1;
            }
        } else {
            // skip bad HTTP header
        }
    }
    return std::make_tuple(httpStatusLine, httpHeaders, httpHeadersCnt);
}

WebSocketUtil::UrlToOriginResult WebSocketUtil::urlToOrigin(const std::string &url) {
    if (boost::to_lower_copy(url) == "null") {
        return std::make_tuple("null", "", boost::none);
    }
    auto res = URLParse::urlSplit(url);
    auto scheme = boost::to_lower_copy(res.getScheme());
    if (scheme == "file") {
        return std::make_tuple("null", "", boost::none);
    }

    auto host = res.getHostName();
    auto port = res.getPort();
    if (!port) {
        if (scheme == "http") {
            port = 80;
        } else if (scheme == "https") {
            port = 443;
        }
    }

    if (!host || host->empty()) {
        NET4CXX_THROW_EXCEPTION(ValueError, StrUtil::format("No host part in Origin '%s'", url));
    }
    return std::make_tuple(scheme, *host, port);
}

bool WebSocketUtil::isSameOrigin(const UrlToOriginResult &websocketOrigin, const std::string &hostScheme,
                                 unsigned short hostPort, const std::vector<boost::regex> &hostPolicy) {
    if (std::get<0>(websocketOrigin) == "null") {
        return false;
    }
    std::string originScheme, originHost;
    boost::optional<unsigned short> originPort;
    std::tie(originScheme, originHost, originPort) = websocketOrigin;
    std::string originHeader;
    if (originPort) {
        originHeader = StrUtil::format("%s://%s:%u", originScheme, originHost, *originPort);
    } else {
        originHeader = StrUtil::format("%s://%s", originScheme, originHost);
    }

    for (auto &originPattern: hostPolicy) {
        if (boost::regex_match(originHeader, originPattern)) {
            return true;
        }
    }
    return false;
}

std::vector<boost::regex> WebSocketUtil::wildcardsToPatterns(const StringVector &wildcards) {
    std::vector<boost::regex> patterns;
    for (auto wc: wildcards) {
        boost::replace_all(wc, ".", "\\.");
        boost::replace_all(wc, "*", ".*");
        wc = "^" + wc + "$";
        patterns.emplace_back(wc);
    }
    return patterns;
}


ByteArray WebSocketUtil::newid(size_t length) {
    ByteArray temp;
    auto l = (size_t)ceil(length * 6.0 / 8.0);
    temp.resize(l);
    Random::randBytes(temp.data(), temp.size());
    auto result = Base64::b64encode(temp);
    result.resize(length);
    return result;
}


void TrafficStats::reset() {
    _outgoingOctetsWireLevel = 0;
    _outgoingOctetsWebSocketLevel = 0;
    _outgoingOctetsAppLevel = 0;
    _outgoingWebSocketFrames = 0;
    _outgoingWebSocketMessages = 0;

    _incomingOctetsWireLevel = 0;
    _incomingOctetsWebSocketLevel = 0;
    _incomingOctetsAppLevel = 0;
    _incomingWebSocketFrames = 0;
    _incomingWebSocketMessages = 0;

    _preopenOutgoingOctetsWireLevel = 0;
    _preopenIncomingOctetsWireLevel = 0;
}


JSONValue TrafficStats::toJson() const {
    JSONValue result;
    result["outgoingOctetsWireLevel"] = (uint64_t)_outgoingOctetsWireLevel;
    result["outgoingOctetsWebSocketLevel"] = (uint64_t)_outgoingOctetsWebSocketLevel;
    result["outgoingOctetsAppLevel"] = (uint64_t)_outgoingOctetsAppLevel;

    if (_outgoingOctetsAppLevel > 0) {
        result["outgoingCompressionRatio"] = 1.0 * _outgoingOctetsWebSocketLevel / _outgoingOctetsAppLevel;
    } else {
        result["outgoingCompressionRatio"] = nullptr;
    }

    if (_outgoingOctetsWebSocketLevel > 0) {
        result["outgoingWebSocketOverhead"] = 1.0 * (_outgoingOctetsWireLevel - _outgoingOctetsWebSocketLevel) /
                                              _outgoingOctetsWebSocketLevel;
    } else {
        result["outgoingWebSocketOverhead"] = nullptr;
    }

    result["outgoingWebSocketFrames"] = (uint64_t)_outgoingWebSocketFrames;
    result["outgoingWebSocketMessages"] = (uint64_t)_outgoingWebSocketMessages;
    result["preopenOutgoingOctetsWireLevel"] = (uint64_t)_preopenOutgoingOctetsWireLevel;

    result["incomingOctetsWireLevel"] = (uint64_t)_incomingOctetsWireLevel;
    result["incomingOctetsWebSocketLevel"] = (uint64_t)_incomingOctetsWebSocketLevel;
    result["incomingOctetsAppLevel"] = (uint64_t)_incomingOctetsAppLevel;

    if (_incomingOctetsAppLevel > 0) {
        result["incomingCompressionRatio"] = 1.0 * _incomingOctetsWebSocketLevel / _incomingOctetsAppLevel;
    } else {
        result["incomingCompressionRatio"] = nullptr;
    }

    if (_incomingOctetsWebSocketLevel > 0) {
        result["incomingWebSocketOverhead"] = 1.0 * (_incomingOctetsWireLevel - _incomingOctetsWebSocketLevel) /
                                              _incomingOctetsWebSocketLevel;
    } else {
        result["incomingWebSocketOverhead"] = nullptr;
    }

    result["incomingWebSocketFrames"] = (uint64_t)_incomingWebSocketFrames;
    result["incomingWebSocketMessages"] = (uint64_t)_incomingWebSocketMessages;
    result["preopenIncomingOctetsWireLevel"] = (uint64_t)_preopenIncomingOctetsWireLevel;
    return result;
}

std::string TrafficStats::toString() const {
    JSONValue value = toJson();
    std::ostringstream os;
    os << value;
    return os.str();
}


NS_END