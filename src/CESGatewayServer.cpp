#include "CESGatewayServer.h"

#include <algorithm>
#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cwctype>
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
constexpr std::size_t kRequestBodyLimitBytes = 8u * 1024u * 1024u;

bool ContainsJsonContentType(std::wstring const& contentType) {
    auto lower = contentType;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return lower.find(L"application/json") != std::wstring::npos || lower.find(L"+json") != std::wstring::npos;
}
} // namespace

struct CESGatewayServer::Impl {
    explicit Impl(std::wstring prefix, unsigned int workerCount)
        : urlPrefix(std::move(prefix))
        , workerCount(workerCount == 0 ? std::max(1u, std::thread::hardware_concurrency()) : workerCount) {}

    std::wstring urlPrefix;
    unsigned int workerCount{};
    RawHandler rawHandler;
#ifdef _WIN32
    JsonHandler jsonHandler;
    HTTP_SERVER_SESSION_ID serverSession{};
    HTTP_URL_GROUP_ID urlGroup{};
    HANDLE requestQueue{nullptr};
    std::atomic<bool> running{false};
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
    auto body = WideToUtf8(reply.body);

    HTTP_RESPONSE response{};
    response.Version = HTTP_VERSION_1_1;
    response.StatusCode = reply.statusCode;
    response.pReason = reason.c_str();
    response.ReasonLength = static_cast<USHORT>(reason.size());
    response.Headers.KnownHeaders[HttpHeaderContentType].pRawValue = contentType.c_str();
    response.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength = static_cast<USHORT>(contentType.size());

    HTTP_DATA_CHUNK chunk{};
    chunk.DataChunkType = HttpDataChunkFromMemory;
    chunk.FromMemory.pBuffer = body.empty() ? nullptr : const_cast<char*>(body.data());
    chunk.FromMemory.BufferLength = static_cast<ULONG>(body.size());
    response.EntityChunkCount = body.empty() ? 0 : 1;
    response.pEntityChunks = body.empty() ? nullptr : &chunk;

    ULONG bytesSent = 0;
    return HttpSendHttpResponse(requestQueue, requestId, 0, &response, nullptr, &bytesSent, nullptr, 0, nullptr, nullptr) == NO_ERROR;
}

std::wstring ReadRequestBody(HANDLE requestQueue, HTTP_REQUEST_ID requestId) {
    std::wstring body;
    std::vector<char> buffer(16 * 1024);

    while (true) {
        ULONG bytesRead = 0;
        ULONG result = HttpReceiveRequestEntityBody(
            requestQueue,
            requestId,
            0,
            buffer.data(),
            static_cast<ULONG>(buffer.size()),
            &bytesRead,
            nullptr);

        if (bytesRead > 0) {
            auto chunk = Utf8ToWide(std::string(buffer.data(), buffer.data() + bytesRead));
            body.append(chunk);
            if (body.size() * sizeof(wchar_t) > kRequestBodyLimitBytes) {
                return {};
            }
        }

        if (result == NO_ERROR) {
            continue;
        }
        if (result == ERROR_HANDLE_EOF) {
            break;
        }
        if (result == ERROR_MORE_DATA) {
            continue;
        }
        return {};
    }

    return body;
}

CESGatewayServer::Request BuildRequest(HTTP_REQUEST const& request, HANDLE requestQueue) {
    CESGatewayServer::Request result;
    result.method = VerbToMethod(request.Verb, request.pUnknownVerb);
    if (request.CookedUrl.pAbsPath != nullptr) {
        result.path = request.CookedUrl.pAbsPath;
    }
    if (request.CookedUrl.pQueryString != nullptr) {
        result.query = request.CookedUrl.pQueryString;
    }
    result.contentType = HeaderValueToWide(HttpHeaderContentType, request);
    result.body = ReadRequestBody(requestQueue, request.RequestId);
    return result;
}

void WorkerLoop(CESGatewayServer::Impl* impl) {
    std::vector<std::max_align_t> buffer((32 * 1024 + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
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
            buffer.resize((bytesReceived + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
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

        auto requestData = BuildRequest(*request, impl->requestQueue);

        CESGatewayServer::Response reply;
        bool handled = false;
        if (impl->jsonHandler && ContainsJsonContentType(requestData.contentType)) {
            auto jsonText = requestData.body;
            if (!jsonText.empty()) {
                winrt::Windows::Data::Json::JsonObject jsonObject;
                if (winrt::Windows::Data::Json::JsonObject::TryParse(winrt::hstring(jsonText), jsonObject)) {
                    reply = impl->jsonHandler(requestData, jsonObject);
                    handled = true;
                } else {
                    reply.statusCode = 400;
                    reply.reasonPhrase = L"Bad Request";
                    reply.contentType = L"application/json; charset=utf-8";
                    reply.body = L"{\"error\":\"invalid json\"}";
                    handled = true;
                }
            }
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

    result = HttpCreateServerSession(HTTPAPI_VERSION_2, &impl_->serverSession, 0);
    if (result != NO_ERROR) {
        setError(L"HttpCreateServerSession failed");
        HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return false;
    }

    result = HttpCreateUrlGroup(impl_->serverSession, &impl_->urlGroup, 0);
    if (result != NO_ERROR) {
        setError(L"HttpCreateUrlGroup failed");
        HttpCloseServerSession(impl_->serverSession);
        HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return false;
    }

    result = HttpCreateRequestQueue(HTTPAPI_VERSION_2, nullptr, nullptr, 0, &impl_->requestQueue);
    if (result != NO_ERROR) {
        setError(L"HttpCreateRequestQueue failed");
        HttpCloseUrlGroup(impl_->urlGroup);
        HttpCloseServerSession(impl_->serverSession);
        HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return false;
    }

    HTTP_BINDING_INFO bindingInfo{};
    bindingInfo.Flags.Present = 1;
    bindingInfo.RequestQueueHandle = impl_->requestQueue;
    result = HttpSetUrlGroupProperty(impl_->urlGroup, HttpServerBindingProperty, &bindingInfo, sizeof(bindingInfo));
    if (result != NO_ERROR) {
        setError(L"HttpSetUrlGroupProperty(HttpServerBindingProperty) failed");
        Stop();
        return false;
    }

    result = HttpAddUrlToUrlGroup(impl_->urlGroup, prefix.c_str(), 0, 0);
    if (result != NO_ERROR) {
        setError(L"HttpAddUrlToUrlGroup failed");
        Stop();
        return false;
    }

    HTTP_ENABLED_STATE enabledState = HttpEnabledStateActive;
    result = HttpSetUrlGroupProperty(impl_->urlGroup, HttpServerStateProperty, &enabledState, sizeof(enabledState));
    if (result != NO_ERROR) {
        setError(L"HttpSetUrlGroupProperty(HttpServerStateProperty) failed");
        Stop();
        return false;
    }

    impl_->running.store(true, std::memory_order_release);
    impl_->workers.reserve(impl_->workerCount);
    try {
        for (unsigned int index = 0; index < impl_->workerCount; ++index) {
            impl_->workers.emplace_back([this]() {
                WorkerLoop(impl_.get());
            });
        }
    } catch (...) {
        setError(L"Failed to start worker threads");
        Stop();
        return false;
    }

    return true;
}

void CESGatewayServer::Stop() {
#ifdef _WIN32
    if (!impl_) {
        return;
    }

    bool wasRunning = impl_->running.exchange(false, std::memory_order_acq_rel);
    if (!wasRunning && impl_->requestQueue == nullptr && impl_->urlGroup == 0 && impl_->serverSession == 0) {
        return;
    }

    if (impl_->requestQueue != nullptr) {
        CloseHandle(impl_->requestQueue);
        impl_->requestQueue = nullptr;
    }

    for (auto& worker : impl_->workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    impl_->workers.clear();

    if (impl_->urlGroup != 0) {
        HttpCloseUrlGroup(impl_->urlGroup);
        impl_->urlGroup = 0;
    }
    if (impl_->serverSession != 0) {
        HttpCloseServerSession(impl_->serverSession);
        impl_->serverSession = 0;
    }
    HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
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
