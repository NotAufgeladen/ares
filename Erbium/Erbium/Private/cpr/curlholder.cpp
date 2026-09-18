#include "cpr/curlholder.h"
#include <cassert>
#include <stdexcept>

namespace cpr {
CurlHolder::CurlHolder() {
    // curl_global_init is completed during Erbium startup. After global
    // initialization, libcurl 7.85 supports concurrent easy-handle creation.
    // CPR's extra function-local std::mutex is unsafe in this injected DLL:
    // its runtime mutex state can be null and crash in MSVCP140!Thrd_yield
    // before curl_easy_init (and therefore before any HTTP request) is called.
    handle = curl_easy_init();

    if (!handle) {
        throw std::runtime_error("cpr: curl_easy_init failed");
    }
} // namespace cpr

CurlHolder::~CurlHolder() {
    curl_slist_free_all(chunk);
    curl_slist_free_all(resolveCurlList);
    curl_mime_free(multipart);
    curl_easy_cleanup(handle);
}

std::string CurlHolder::urlEncode(const std::string& s) const {
    assert(handle);
    char* output = curl_easy_escape(handle, s.c_str(), static_cast<int>(s.length()));
    if (output) {
        std::string result = output;
        curl_free(output);
        return result;
    }
    return "";
}

std::string CurlHolder::urlDecode(const std::string& s) const {
    assert(handle);
    char* output = curl_easy_unescape(handle, s.c_str(), static_cast<int>(s.length()), nullptr);
    if (output) {
        std::string result = output;
        curl_free(output);
        return result;
    }
    return "";
}
} // namespace cpr
