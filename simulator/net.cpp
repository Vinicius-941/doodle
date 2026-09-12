#include "net.h"

#include <windows.h>
#include <winhttp.h>

#include <stdexcept>
#include <string>

namespace {

struct Handle {  // WinHTTP devolve handles que precisam fechar em qualquer saída
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

[[noreturn]] void fail(const std::string& what, const std::string& url) {
    throw std::runtime_error(what + " (" + url + ")");
}

}  // namespace

std::string net::get(const std::string& url, size_t maxBytes) {
    std::wstring wurl = widen(url);
    wchar_t host[256] = {}, path[2048] = {};
    URL_COMPONENTS uc = {sizeof uc};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) fail("endereço inválido", url);

    // Uma sessão só para o processo inteiro: abrir uma por arquivo refaz a descoberta de proxy a cada download.
    static Handle session{WinHttpOpen(L"Doodle", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) fail("sem rede", url);
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 20000);  // o console não pode ficar preso num servidor mudo
    Handle conn{WinHttpConnect(session, host, uc.nPort, 0)};
    if (!conn) fail("não conectou", url);
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle req{WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (!req) fail("não abriu o pedido", url);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr))
        fail("servidor não respondeu", url);

    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) fail("o servidor respondeu " + std::to_string(status), url);

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) fail("download interrompido", url);
        if (avail == 0) break;
        if (body.size() + avail > maxBytes) fail("arquivo grande demais", url);
        size_t at = body.size();
        body.resize(at + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, body.data() + at, avail, &read)) fail("download interrompido", url);
        body.resize(at + read);
    }
    return body;
}
