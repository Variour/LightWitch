#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

#include <functional>

// The shape every API endpoint is written against, regardless of which
// transport carried the request in — HTTP (BatteryWebServer) or USB Serial
// (SerialConfigServer). A handler is registered once (see BatteryWebServer::_get/
// _post/_postStream) and both transports call the exact same function, so a
// new endpoint only has to be written once to work over both.
enum class ApiMethod : uint8_t { GET = 0, POST = 1 };

struct ApiRequest {
    ApiMethod method = ApiMethod::GET;
    const char* path = "";

    // Parsed JSON body. Null for GET requests and for streaming requests
    // (see chunk fields below) — those carry their payload as raw bytes instead.
    JsonVariantConst body;

    // Single query-string parameter, used only by GET /api/scenes/get?id=...
    // — the one endpoint that takes a parameter outside the JSON body.
    const char* query = nullptr;

    // Streaming-body endpoints only (scene save, storage upload): the chunk
    // just delivered and its position within the overall body. A handler
    // registered via _postStream is called once per chunk; chunkIndex==0 is
    // the first call, chunkIndex+chunkLen==chunkTotal is the last.
    const uint8_t* chunk = nullptr;
    size_t chunkLen = 0;
    size_t chunkIndex = 0;
    size_t chunkTotal = 0;

    // Per-request scratch state a streaming handler allocates on the first
    // chunk and frees on the last — analogous to AsyncWebServerRequest's
    // _tempObject, but transport-neutral. The handler both reads and writes
    // this field; the transport adapter is just responsible for persisting
    // it between calls for the same underlying connection/request and for
    // handing it back on the next chunk.
    void* streamState = nullptr;
};

struct ApiResponse {
    int status = 200;
    JsonDocument body;

    // Set instead of `body` when the response is a raw file straight off
    // LittleFS (GET /api/scenes/get) rather than a JSON object. A String
    // (not const char*) because the path is normally built from a local
    // temporary that must survive until the transport adapter reads it.
    String rawFilePath;

    // Handler wants ESP.restart() after the response has been flushed —
    // both transports must send the response first, then restart.
    bool restart = false;

    // Streaming endpoints only: false on every chunk except the last, so the
    // transport knows not to send a response yet.
    bool streamDone = true;
};

using ApiHandler = std::function<void(ApiRequest&, ApiResponse&)>;

struct ApiRoute {
    ApiMethod method;
    const char* path;
    ApiHandler handler;
    bool streaming = false;  // true for scene save / storage upload
};
