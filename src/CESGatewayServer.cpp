#include "CESGatewayServer.h"

#include <algorithm>
#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <http.h>
#include <winrt/base.h>
#pragma comment(lib, "httpapi.lib")
#endif

namespace {
// Cap request bodies to keep memory usage bounded under heavy load.
constexpr std::size_t kRequestBodyLimitBytes = 8u * 1024u * 1024u;
constexpr std::size_t kRequestBodyChunkBytes = 16u * 1024u;
constexpr std::size_t kInitialRequestBufferBytes = 32u * 1024u;

std::size_t AlignedBufferUnits(std::size_t bytes) {
    return (bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
}

bool ContainsJsonContentType(std::wstring const& contentType) {
    auto lower = contentType;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return lower.find(L"application/json") != std::wstring::npos || lower.find(L"+json") != std::wstring::npos;
}
} // namespace

namespace {
enum class RequestBodyStatus {
    Ok,
    TooLarge,
    Error,
};

struct RequestBodyResult {
    std::wstring body;
    RequestBodyStatus status{RequestBodyStatus::Ok};
};
} // namespace

struct CESGatewayServer::Impl {
    explicit Impl(std::wstring prefix, unsigned int workerCount)
        : urlPrefix(std::move(prefix))
        , workerCount(workerCount == 0 ? std::max(1u, std::thread::hardware_concurrency()) : workerCount) {}

    std::wstring urlPrefix;
    unsigned int workerCount{};
    RawHandler rawHandler;
    std::mutex lifecycleMutex;
#ifdef _WIN32
    JsonHandler jsonHandler;
    HTTP_SERVER_SESSION_ID serverSession{};
    HTTP_URL_GROUP_ID urlGroup{};
    HANDLE requestQueue{nullptr};
    std::atomic<bool> running{false};
    bool httpInitialized{false};
    std::vector<std::thread> workers;
#endif
};

CESGatewayServer::CESGatewayServer(std::wstring urlPrefix, unsigned int workerCount)
    : impl_(std::make_unique<Impl>(std::move(urlPrefix), workerCount)) {}

CESGatewayServer::~CESGatewayServer() {
    Stop();
}

void CESGatewayServer::SetHandler(RawHandler handler) {
    impl_->rawHandler = std::move(handler);
}

#ifdef _WIN32
void CESGatewayServer::SetJsonHandler(JsonHandler handler) {
    impl_->jsonHandler = std::move(handler);
}
#endif

bool CESGatewayServer::IsRunning() const noexcept {
#ifdef _WIN32
    return impl_ && impl_->running.load(std::memory_order_acquire);
#else
    return false;
#endif
}

#ifdef _WIN32
namespace {

std::wstring Utf8ToWide(std::string const& input) {
    if (input.empty()) {
        return {};
    }
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), output.data(), needed);
    return output;
}

std::string WideToUtf8(std::wstring const& input) {
    if (input.empty()) {
        return {};
    }
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), needed, nullptr, nullptr);
    return output;
}

std::wstring VerbToMethod(HTTP_VERB verb, PCSTR unknownVerb) {
    switch (verb) {
    case HttpVerbGET: return L"GET";
    case HttpVerbPUT: return L"PUT";
    case HttpVerbPOST: return L"POST";
    case HttpVerbDELETE: return L"DELETE";
    case HttpVerbHEAD: return L"HEAD";
    case HttpVerbOPTIONS: return L"OPTIONS";
    case HttpVerbTRACE: return L"TRACE";
    case HttpVerbCONNECT: return L"CONNECT";
    case HttpVerbPATCH: return L"PATCH";
    default:
        return unknownVerb ? Utf8ToWide(std::string{unknownVerb}) : L"UNKNOWN";
    }
}

std::wstring HeaderValueToWide(HTTP_HEADER_ID id, HTTP_REQUEST const& request) {
    auto const& header = request.Headers.KnownHeaders[id];
    if (header.pRawValue == nullptr || header.RawValueLength == 0) {
        return {};
    }
    return Utf8ToWide(std::string{header.pRawValue, header.RawValueLength});
}

std::wstring BuildReasonPhrase(std::uint16_t statusCode, std::wstring const& reasonPhrase) {
    if (!reasonPhrase.empty()) {
        return reasonPhrase;
    }
    switch (statusCode) {
    case 200: return L"OK";
    case 400: return L"Bad Request";
    case 404: return L"Not Found";
    case 405: return L"Method Not Allowed";
    case 413: return L"Payload Too Large";
    case 500: return L"Internal Server Error";
    default: return L"OK";
    }
}

bool SendResponse(HANDLE requestQueue, HTTP_REQUEST_ID requestId, CESGatewayServer::Response const& reply) {
    auto reason = WideToUtf8(BuildReasonPhrase(reply.statusCode, reply.reasonPhrase));
    auto contentType = WideToUtf8(reply.contentType);
    auto bodyBytes = WideToUtf8(reply.body);

    HTTP_RESPONSE response{};
    response.Version = HTTP_VERSION_1_1;
    response.StatusCode = reply.statusCode;
    response.pReason = reason.c_str();
    response.ReasonLength = static_cast<USHORT>(reason.size());
    response.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = contentType.c_str();
    response.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = static_cast<USHORT>(contentType.size());

    HTTP_DATA_CHUNK chunk{};
    if (!bodyBytes.empty()) {
        chunk.DataChunkType = HttpDataChunkFromMemory;
        chunk.FromMemory.pBuffer = bodyBytes.data();
        chunk.FromMemory.BufferLength = static_cast<ULONG>(bodyBytes.size());
        response.EntityChunkCount = 1;
        response.pEntityChunks = &chunk;
    }

    ULONG bytesSent = 0;
    return HttpSendHttpResponse(requestQueue, requestId, 0, &response, nullptr, &bytesSent, nullptr, 0, nullptr, nullptr) == NO_ERROR;
}

RequestBodyResult ReadRequestBody(HANDLE requestQueue, HTTP_REQUEST_ID requestId) {
    RequestBodyResult result;
    std::vector<char> buffer(kRequestBodyChunkBytes);
    std::size_t bytesAccumulated = 0;

    while (true) {
        ULONG bytesRead = 0;
        ULONG apiResult = HttpReceiveRequestEntityBody(
            requestQueue,
            requestId,
            0,
            buffer.data(),
            static_cast<ULONG>(buffer.size()),
            &bytesRead,
            nullptr);

        if (bytesRead > 0) {
            bytesAccumulated += bytesRead;
            if (bytesAccumulated > kRequestBodyLimitBytes) {
                result.status = RequestBodyStatus::TooLarge;
                return result;
            }
            auto chunk = Utf8ToWide(std::string(buffer.data(), buffer.data() + bytesRead));
            result.body.append(chunk);
        }

        if (apiResult == ERROR_HANDLE_EOF) {
            break;
        }
        if (apiResult == NO_ERROR || apiResult == ERROR_MORE_DATA) {
            continue;
        }
        result.status = RequestBodyStatus::Error;
        return result;
    }

    return result;
}

struct BuiltRequest {
    CESGatewayServer::Request request;
    RequestBodyStatus bodyStatus{RequestBodyStatus::Ok};
};

BuiltRequest BuildRequest(HTTP_REQUEST const& request, HANDLE requestQueue) {
    BuiltRequest result;
    result.request.method = VerbToMethod(request.Verb, request.pUnknownVerb);
    if (request.CookedUrl.pAbsPath != nullptr) {
        result.request.path = request.CookedUrl.pAbsPath;
    }
    if (request.CookedUrl.pQueryString != nullptr) {
        result.request.query = request.CookedUrl.pQueryString;
    }
    result.request.contentType = HeaderValueToWide(HttpHeaderContentType, request);
    auto body = ReadRequestBody(requestQueue, request.RequestId);
    result.request.body = std::move(body.body);
    result.bodyStatus = body.status;
    return result;
}

void CleanupServer(CESGatewayServer::Impl& impl) {
    impl.running.exchange(false, std::memory_order_acq_rel);

    if (impl.requestQueue != nullptr) {
        // Closing the queue unblocks workers so they can observe shutdown and exit cleanly.
        HttpCloseRequestQueue(impl.requestQueue);
        impl.requestQueue = nullptr;
    }

    for (auto& worker : impl.workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    impl.workers.clear();

    if (impl.urlGroup != 0) {
        HttpCloseUrlGroup(impl.urlGroup);
        impl.urlGroup = 0;
    }
    if (impl.serverSession != 0) {
        HttpCloseServerSession(impl.serverSession);
        impl.serverSession = 0;
    }
    if (impl.httpInitialized) {
        HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        impl.httpInitialized = false;
    }
}

void WorkerLoop(CESGatewayServer::Impl* impl) {
    std::vector<std::max_align_t> buffer(AlignedBufferUnits(kInitialRequestBufferBytes));
    while (impl->running.load(std::memory_order_acquire)) {
        auto* request = reinterpret_cast<PHTTP_REQUEST>(buffer.data());
        ULONG bytesReceived = 0;
        ULONG result = HttpReceiveHttpRequest(
            impl->requestQueue,
            0,
            0,
            request,
            static_cast<ULONG>(buffer.size() * sizeof(std::max_align_t)),
            &bytesReceived,
            nullptr);

        if (result == ERROR_MORE_DATA) {
            buffer.resize(AlignedBufferUnits(bytesReceived));
            request = reinterpret_cast<PHTTP_REQUEST>(buffer.data());
            result = HttpReceiveHttpRequest(
                impl->requestQueue,
                0,
                0,
                request,
                static_cast<ULONG>(buffer.size() * sizeof(std::max_align_t)),
                &bytesReceived,
                nullptr);
        }

        if (result != NO_ERROR) {
            if (!impl->running.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }

        auto builtRequest = BuildRequest(*request, impl->requestQueue);
        auto& requestData = builtRequest.request;

        CESGatewayServer::Response reply;
        bool handled = false;
        bool expectsJson = impl->jsonHandler && ContainsJsonContentType(requestData.contentType);
        if (builtRequest.bodyStatus == RequestBodyStatus::TooLarge) {
            reply.statusCode = 413;
            reply.reasonPhrase = L"Payload Too Large";
            reply.contentType = L"application/json; charset=utf-8";
            reply.body = L"{\"error\":\"request body too large\"}";
            handled = true;
        } else if (builtRequest.bodyStatus == RequestBodyStatus::Error) {
            reply.statusCode = 400;
            reply.reasonPhrase = L"Bad Request";
            reply.contentType = L"application/json; charset=utf-8";
            reply.body = L"{\"error\":\"failed to read request body\"}";
            handled = true;
        } else if (expectsJson && !requestData.body.empty()) {
            if (winrt::Windows::Data::Json::JsonObject jsonObject;
                winrt::Windows::Data::Json::JsonObject::TryParse(winrt::hstring(requestData.body), jsonObject)) {
                reply = impl->jsonHandler(requestData, jsonObject);
                handled = true;
            } else {
                reply.statusCode = 400;
                reply.reasonPhrase = L"Bad Request";
                reply.contentType = L"application/json; charset=utf-8";
                reply.body = L"{\"error\":\"invalid JSON\"}";
                handled = true;
            }
        } else if (expectsJson) {
            reply.statusCode = 400;
            reply.reasonPhrase = L"Bad Request";
            reply.contentType = L"application/json; charset=utf-8";
            reply.body = L"{\"error\":\"empty JSON body\"}";
            handled = true;
        }

        if (!handled && impl->rawHandler) {
            reply = impl->rawHandler(requestData);
            handled = true;
        }

        if (!handled) {
            reply.statusCode = 404;
            reply.reasonPhrase = L"Not Found";
            reply.contentType = L"application/json; charset=utf-8";
            reply.body = L"{\"error\":\"no handler registered\"}";
        }

        SendResponse(impl->requestQueue, request->RequestId, reply);
    }
}

std::wstring NormalizePrefix(std::wstring prefix) {
    if (prefix.empty()) {
        return L"https://+:8443/";
    }
    if (prefix.back() != L'/') {
        prefix.push_back(L'/');
    }
    return prefix;
}

} // namespace

bool CESGatewayServer::Start(std::wstring* errorMessage) {
    std::lock_guard<std::mutex> lock(impl_->lifecycleMutex);

    if (IsRunning()) {
        return true;
    }

    auto prefix = NormalizePrefix(impl_->urlPrefix);

    auto setError = [errorMessage](std::wstring message) {
        if (errorMessage != nullptr) {
            *errorMessage = std::move(message);
        }
    };

    ULONG result = HttpInitialize(HTTPAPI_VERSION_2, HTTP_INITIALIZE_SERVER, nullptr);
    if (result != NO_ERROR) {
        setError(L"HttpInitialize failed");
        return false;
    }
    impl_->httpInitialized = true;

    result = HttpCreateServerSession(HTTPAPI_VERSION_2, &impl_->serverSession, 0);
    if (result != NO_ERROR) {
        setError(L"HttpCreateServerSession failed");
        CleanupServer(*impl_);
        return false;
    }

    result = HttpCreateUrlGroup(impl_->serverSession, &impl_->urlGroup, 0);
    if (result != NO_ERROR) {
        setError(L"HttpCreateUrlGroup failed");
        CleanupServer(*impl_);
        return false;
    }

    result = HttpCreateRequestQueue(HTTPAPI_VERSION_2, nullptr, nullptr, 0, &impl_->requestQueue);
    if (result != NO_ERROR) {
        setError(L"HttpCreateRequestQueue failed");
        CleanupServer(*impl_);
        return false;
    }

    HTTP_BINDING_INFO bindingInfo{};
    bindingInfo.Flags.Present = 1;
    bindingInfo.RequestQueueHandle = impl_->requestQueue;
    result = HttpSetUrlGroupProperty(impl_->urlGroup, HttpServerBindingProperty, &bindingInfo, sizeof(bindingInfo));
    if (result != NO_ERROR) {
        setError(L"HttpSetUrlGroupProperty(HttpServerBindingProperty) failed");
        CleanupServer(*impl_);
        return false;
    }

    result = HttpAddUrlToUrlGroup(impl_->urlGroup, prefix.c_str(), 0, 0);
    if (result != NO_ERROR) {
        setError(L"HttpAddUrlToUrlGroup failed");
        CleanupServer(*impl_);
        return false;
    }

    HTTP_ENABLED_STATE enabledState = HttpEnabledStateActive;
    result = HttpSetUrlGroupProperty(impl_->urlGroup, HttpServerStateProperty, &enabledState, sizeof(enabledState));
    if (result != NO_ERROR) {
        setError(L"HttpSetUrlGroupProperty(HttpServerStateProperty) failed");
        CleanupServer(*impl_);
        return false;
    }

    impl_->running.store(true, std::memory_order_release);
    impl_->workers.reserve(impl_->workerCount);
    try {
        for (unsigned int i = 0; i < impl_->workerCount; ++i) {
            impl_->workers.emplace_back([this]() {
                WorkerLoop(impl_.get());
            });
        }
    } catch (...) {
        setError(L"Failed to start worker threads");
        CleanupServer(*impl_);
        return false;
    }

    return true;
}

void CESGatewayServer::Stop() {
#ifdef _WIN32
    if (!impl_) {
        return;
    }

    std::lock_guard<std::mutex> lock(impl_->lifecycleMutex);
    CleanupServer(*impl_);
#endif
}

#endif // _WIN32

#ifndef _WIN32
bool CESGatewayServer::Start(std::wstring* errorMessage) {
    if (errorMessage != nullptr) {
        *errorMessage = L"CESGatewayServer is only supported on Windows.";
    }
    return false;
}
#endif
