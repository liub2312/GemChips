#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <winrt/Windows.Data.Json.h>
#endif

class CESGatewayServer {
public:
    struct Request {
        std::wstring method;
        std::wstring path;
        std::wstring query;
        std::wstring contentType;
        std::wstring body;
    };

    struct Response {
        std::uint16_t statusCode{200};
        std::wstring reasonPhrase{L"OK"};
        std::wstring contentType{L"application/json; charset=utf-8"};
        std::wstring body;
    };

    using RawHandler = std::function<Response(Request const&)>;

#ifdef _WIN32
    using JsonHandler = std::function<Response(Request const&, winrt::Windows::Data::Json::JsonObject const&)>;
#endif

    // workerCount == 0 uses std::thread::hardware_concurrency() as the default.
    explicit CESGatewayServer(std::wstring urlPrefix, unsigned int workerCount = 0);
    ~CESGatewayServer();

    CESGatewayServer(CESGatewayServer const&) = delete;
    CESGatewayServer& operator=(CESGatewayServer const&) = delete;
    CESGatewayServer(CESGatewayServer&&) = delete;
    CESGatewayServer& operator=(CESGatewayServer&&) = delete;

    void SetHandler(RawHandler handler);

#ifdef _WIN32
    void SetJsonHandler(JsonHandler handler);
#endif

    bool Start(std::wstring* errorMessage = nullptr);
    void Stop();
    bool IsRunning() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
