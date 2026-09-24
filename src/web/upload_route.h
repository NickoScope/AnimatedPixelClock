// ============================================================
// upload_route.h - a POST route that takes a multipart file and nothing else
// ============================================================
// WebServer's server.on(uri, method, done, chunk) hands `chunk` two kinds of
// body: a multipart file, through upload(), and ANY other POST body, through
// raw() (FunctionRequestHandler, arduino-esp32 2.0.17, RequestHandlersImpl.h).
// Every chunk handler here reads server.upload(), which during a raw body is a
// reference through a null pointer. On 2026-09-24 one `curl --data-binary` to
// /api/lua/upload crashed the panel with LoadProhibited in
// handleLuaUploadChunk, and the new firmware was rolled back for it: any
// machine on the network could do the same to any of the four upload routes.
//
// Here a raw body is read and thrown away as it arrives, 1.4 KB at a time
// (the library's own loop; nothing is kept), `chunk` never sees it, and only
// `done` runs - which answers "no file" when no upload started.
// ============================================================
#ifndef UPLOAD_ROUTE_H
#define UPLOAD_ROUTE_H

#include <WebServer.h>

class UploadRoute : public RequestHandler {
 public:
  UploadRoute(const char *uri, WebServer::THandlerFunction done, WebServer::THandlerFunction chunk)
      : uri_(uri), done_(done), chunk_(chunk) {}

  bool canHandle(HTTPMethod method, String uri) override {
    return method == HTTP_POST && uri == uri_;
  }
  bool canUpload(String uri) override { return uri == uri_; }
  bool canRaw(String uri) override { return uri == uri_; }   // taken, to be dropped
  bool handle(WebServer &, HTTPMethod method, String uri) override {
    if (!canHandle(method, uri)) return false;
    done_();
    return true;
  }
  void upload(WebServer &, String uri, HTTPUpload &) override {
    if (canUpload(uri)) chunk_();
  }
  void raw(WebServer &, String, HTTPRaw &) override {}

 private:
  const char *uri_;
  WebServer::THandlerFunction done_, chunk_;
};

// server.on(uri, HTTP_POST, done, chunk), without the raw path into chunk.
inline void serverOnUpload(WebServer &s, const char *uri, WebServer::THandlerFunction done,
                           WebServer::THandlerFunction chunk) {
  s.addHandler(new UploadRoute(uri, done, chunk));
}

#endif
