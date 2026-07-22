#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <mbedtls/base64.h>

#include "../config/Config.h"
#include "../logging/Logger.h"
#include "../version.h"
#include "ApiTypes.h"
#include "WebServer.h"

// USB-serial config transport for #355: lets a browser (via the Web Serial
// API, from a page that isn't served by this device — the whole point is
// working without WiFi or the device's AP) configure this device over the
// same physical Serial connection normally used for log output.
//
// Every endpoint is reached through BatteryWebServer::dispatch(), the exact
// table the HTTP API is built from (see ApiTypes.h) — this class is purely a
// framing/transport layer and contains no endpoint logic of its own. A new
// endpoint registered on BatteryWebServer automatically works here too.
//
// Wire protocol: newline-delimited frames, each prefixed with "@BLCFG " so
// they can never be mistaken for a plain-text Logger line. While a session
// is active (from HELLO to BYE, or an idle timeout) the Logger sink to
// Serial is suspended (see sessionActive(), consulted by main.cpp's
// serialSink) so the two can't interleave on the wire.
//
//   Browser -> device:
//     @BLCFG HELLO
//     @BLCFG REQ {"id":1,"method":"GET","path":"/api/config"}
//     @BLCFG REQ {"id":2,"method":"POST","path":"/api/wifi/add","body":{...}}
//     @BLCFG REQ {"id":3,"method":"POST","path":"/api/storage/upload",
//                 "query":"song.wav","index":0,"total":123,"chunk":"<base64>"}
//     @BLCFG BYE
//
//   Device -> browser:
//     @BLCFG READY {"proto":1,"device":"<name>","version":"<fw>"}
//     @BLCFG RES {"id":1,"status":200,"body":{...}}
//     @BLCFG RES {"id":3,"ack":true,"index":0}                      (mid-upload)
//     @BLCFG RES {"id":4,"status":200,"index":0,"total":9000,"chunk":"<base64>","final":false}
//     @BLCFG BYE
//
// A streaming request's response is only sent once, after its final chunk
// has been dispatched (mirrors the HTTP side, see BatteryWebServer::
// _postStream). A response whose own body is large (GET /api/scenes/get, a
// big scene) is sent back the same chunked way instead of one inline frame.
//
// Single request in flight at a time — the browser must wait for a
// response (or upload ack) before sending the next request/chunk. This
// matches one browser tab talking to one device over one serial port; it is
// not a pipelined/concurrent protocol.
class SerialConfigServer {
   public:
    // Resumes plain-text logging if the browser disappears mid-session
    // (tab closed, USB unplugged) without a clean BYE.
    static constexpr uint32_t SESSION_IDLE_TIMEOUT_MS = 30000;
    // Raw (pre-base64) size of one upload/download chunk. Matched by the
    // frontend transport, which must never send a larger chunk than this.
    static constexpr size_t CHUNK_BYTES = 2048;

    void begin(BatteryWebServer* webServer) { _web = webServer; }

    // Consulted by main.cpp's serialSink so plain log lines never interleave
    // with protocol frames on the wire.
    bool sessionActive() const { return _sessionActive; }

    void loop() {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\n') {
                _handleLine(_line);
                _line = "";
            } else if (c != '\r') {
                if (_line.length() < MAX_LINE)
                    _line += c;
                else
                    _line = "";  // runaway line (e.g. a stray non-protocol byte storm) — drop it
            }
        }
        if (_sessionActive && millis() - _lastActivityMs > SESSION_IDLE_TIMEOUT_MS) {
            Logger::w("[serialcfg] session idle timeout, resuming normal logging");
            _endSession();
        }
    }

   private:
    // One base64 chunk line (~4/3 expansion of CHUNK_BYTES) plus JSON framing.
    static constexpr size_t MAX_LINE = 6144;
    static constexpr const char* PREFIX = "@BLCFG ";

    BatteryWebServer* _web = nullptr;
    String _line;
    bool _sessionActive = false;
    uint32_t _lastActivityMs = 0;

    // In-flight streaming upload (scene save / storage upload) state, owned
    // by whichever handler allocated it (see ApiRequest::streamState).
    String _streamPath;
    void* _streamState = nullptr;

    void _endSession() {
        // An interrupted upload leaks its handler-owned state (SceneSaveState/
        // StorageUploadState) rather than risk guessing its type to free it
        // here — acceptable for the rare "connection dropped mid-upload"
        // case; the target file is simply left partially written, same as an
        // interrupted HTTP upload would be.
        _streamState = nullptr;
        _streamPath = "";
        _sessionActive = false;
    }

    void _send(const String& frame) {
        Serial.print(PREFIX);
        Serial.print(frame);
        Serial.print('\n');
    }

    void _handleLine(const String& line) {
        if (!line.startsWith(PREFIX)) return;  // stray log/noise line, ignore
        _lastActivityMs = millis();
        String rest = line.substring(strlen(PREFIX));

        if (rest == "HELLO") {
            _sessionActive = true;
            JsonDocument doc;
            doc["proto"] = 1;
            doc["device"] = Config::get().deviceName;
            doc["version"] = FW_VERSION;
            String body;
            serializeJson(doc, body);
            _send("READY " + body);
            return;
        }
        if (rest == "BYE") {
            _endSession();
            _send("BYE");
            return;
        }
        if (!_sessionActive) return;  // ignore REQ frames outside a session
        if (!rest.startsWith("REQ ")) return;

        JsonDocument doc;
        if (deserializeJson(doc, rest.substring(4))) {
            Logger::w("[serialcfg] bad request frame");
            return;
        }
        _handleRequest(doc);
    }

    void _handleRequest(JsonDocument& doc) {
        uint32_t id = doc["id"] | (uint32_t)0;
        const char* method = doc["method"] | "GET";
        String path = doc["path"] | "";

        ApiMethod m = strcmp(method, "POST") == 0 ? ApiMethod::POST : ApiMethod::GET;
        bool streaming = _web->isStreamingRoute(m, path.c_str());

        ApiRequest req;
        req.method = m;
        req.path = path.c_str();
        ApiResponse resp;

        String query = doc["query"] | "";
        if (query.length()) req.query = query.c_str();

        uint8_t chunkBuf[CHUNK_BYTES];
        if (streaming) {
            const char* b64 = doc["chunk"] | "";
            size_t outLen = 0;
            if (b64[0] && mbedtls_base64_decode(chunkBuf, sizeof(chunkBuf), &outLen,
                                                (const unsigned char*)b64, strlen(b64)) != 0) {
                Logger::w("[serialcfg] bad base64 chunk, dropping request id=%u", (unsigned)id);
                return;
            }
            req.chunk = chunkBuf;
            req.chunkLen = outLen;
            req.chunkIndex = doc["index"] | (size_t)0;
            req.chunkTotal = doc["total"] | (size_t)0;
            req.streamState = (path == _streamPath) ? _streamState : nullptr;
            resp.streamDone = false;
        }

        bool found = _web->dispatch(req, resp);
        if (!found) {
            resp.status = 404;
            resp.body.clear();
            resp.body["error"] = "not found";
            resp.streamDone = true;
        }

        if (streaming) {
            _streamState = req.streamState;
            _streamPath = _streamState ? path : String("");
            if (!resp.streamDone) {
                // Lightweight ack so the browser paces the next chunk instead
                // of sending the whole upload without any flow control.
                JsonDocument ack;
                ack["id"] = id;
                ack["ack"] = true;
                ack["index"] = req.chunkIndex;
                String body;
                serializeJson(ack, body);
                _send("RES " + body);
                return;
            }
        }

        _sendResponse(id, resp);
        if (resp.restart) {
            delay(200);
            ESP.restart();
        }
    }

    void _sendResponse(uint32_t id, ApiResponse& resp) {
        if (resp.rawFilePath.length()) {
            _sendFileResponse(id, resp);
            return;
        }
        String bodyStr;
        serializeJson(resp.body, bodyStr);
        // Comfortably true for every config/list endpoint; only raw scene
        // files (handled above) and pathologically large scene JSON bodies
        // need the chunked form.
        if (bodyStr.length() < CHUNK_BYTES) {
            JsonDocument doc;
            doc["id"] = id;
            doc["status"] = resp.status;
            doc["body"] = resp.body;
            String out;
            serializeJson(doc, out);
            _send("RES " + out);
        } else {
            _sendChunked(id, resp.status, bodyStr);
        }
    }

    void _sendFileResponse(uint32_t id, ApiResponse& resp) {
        File f = LittleFS.open(resp.rawFilePath, "r");
        if (!f) {
            JsonDocument doc;
            doc["id"] = id;
            doc["status"] = 500;
            doc["body"]["error"] = "file open failed";
            String out;
            serializeJson(doc, out);
            _send("RES " + out);
            return;
        }
        String content = f.readString();
        f.close();
        _sendChunked(id, 200, content);
    }

    // Sends `content` as one or more base64-encoded "RES" frames of at most
    // CHUNK_BYTES raw bytes each — the same chunk/index/total/final shape a
    // streaming request uses on the way in. The frontend transport
    // reassembles these into a single response body once final:true arrives.
    void _sendChunked(uint32_t id, int status, const String& content) {
        size_t total = content.length();
        size_t sent = 0;
        uint8_t outBuf[CHUNK_BYTES * 2 + 16];  // base64 expands ~4/3; generous margin
        do {
            size_t n = min(CHUNK_BYTES, total - sent);
            size_t outLen = 0;
            mbedtls_base64_encode(outBuf, sizeof(outBuf), &outLen,
                                  (const unsigned char*)content.c_str() + sent, n);
            outBuf[outLen] = '\0';
            JsonDocument doc;
            doc["id"] = id;
            doc["status"] = status;
            doc["index"] = sent;
            doc["total"] = total;
            doc["chunk"] = (const char*)outBuf;
            sent += n;
            doc["final"] = sent >= total;
            String out;
            serializeJson(doc, out);
            _send("RES " + out);
        } while (sent < total);
    }
};
