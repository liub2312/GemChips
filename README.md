# TGImageTrans

## CESGatewayServer

This repository now includes a Windows-only `CESGatewayServer` class for accepting HTTPS requests through the Windows HTTP Server API.

- Supports multiple worker threads for concurrent request handling
- Parses JSON bodies with `winrt::Windows::Data::Json::JsonObject`
- Exposes a raw handler and a JSON-aware handler on Windows

HTTPS certificate binding must be configured on the machine before the server can accept traffic.