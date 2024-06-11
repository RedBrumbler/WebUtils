#include "DownloaderUtility.hpp"
#include "logging.hpp"

#include "libcurl/shared/curl.h"
#include "libcurl/shared/easy.h"
#include <fmt/core.h>
#include <filesystem>
#include <fstream>

namespace WebUtils {
    std::pair<char, char> getByteChars(char c) {
        static char nibbleToChar[] = "0123456789abcdef";
        auto lower = c & 0b1111;
        auto upper = (c >> 4) & 0b1111;

        return {nibbleToChar[lower], nibbleToChar[upper]};
    }

    std::string escape(std::string_view url) {
        static char forbidden[] = "@&;:<>=?\"'\\!#%+$,{}|^[]`";
        static auto forbiddenEnd = forbidden + (sizeof(forbidden) / sizeof(char));
        static auto isForbidden = [](char c) { return std::find(forbidden, forbiddenEnd, c) != forbiddenEnd; };

        std::string escaped;
        escaped.reserve(url.size());

        for (auto c : url) {
            if (isForbidden(c)) {
                escaped.push_back(u'%');
                // we lose width here but all forbidden chars are only as big as 1 byte (char) anyway
                auto [lc, uc] = getByteChars(static_cast<char>(c));
                escaped.push_back(uc);
                escaped.push_back(lc);
            } else {
                escaped.push_back(c);
            }
        }

        return escaped;
    }

    std::string URLOptions::fullURl() const {
        auto protocol = this->protocol();
        auto afterProtocol = url.substr(protocol.size() + 3);
        if (!noEscape) afterProtocol = escape(afterProtocol);
        if (queries.empty()) return fmt::format("{}://{}", protocol, afterProtocol);

        std::vector<std::string> formattedQueries;
        if (noEscape) {
            for (auto& [key, value] : queries) formattedQueries.emplace_back(fmt::format("{}={}", key, value));
        } else {
            for (auto& [key, value] : queries) formattedQueries.emplace_back(fmt::format("{}={}", escape(key), escape(value)));
        }

        return fmt::format("{}://{}?{}", protocol, afterProtocol, fmt::join(formattedQueries, "&"));
    }

    std::string_view URLOptions::protocol() const {
        auto divider = url.find("://");
        if (divider == std::string::npos) return {};
        return {url.c_str(), divider};
    }

    static std::size_t write_vec_cb(uint8_t* content, std::size_t size, std::size_t nmemb, std::vector<uint8_t>* vec) {
        std::span<uint8_t> addedData(content, (size * nmemb));
        vec->insert(std::next(vec->begin(), vec->size()), addedData.begin(), addedData.end());
        return addedData.size();
    };

    static std::size_t write_str_cb(char* content, std::size_t size, std::size_t nmemb, std::string* str) {
        std::string_view addedText(content, (size * nmemb));
        str->append(addedText);
        return addedText.size();
    };

    bool DownloaderUtility::GetInto(URLOptions urlOptions, IResponse* response, std::function<void(float)> progressReport) const {
        if (!response) return false;

        // if the url is for a filepath, read it from disk instead
        if (urlOptions.isFileURL()) {
            response->CurlStatus = 0;
            std::filesystem::path filePath(urlOptions.url.substr(7));
            if (std::filesystem::exists(filePath) && filePath.has_filename()) {
                std::ifstream file(filePath, std::ios::ate | std::ios::binary | std::ios::in);
                std::vector<uint8_t> data(file.tellg());
                file.seekg(0, std::ios::beg);
                file.read((char*)data.data(), data.size());
                response->AcceptData(data);
                return response->IsSuccessful() && response->DataParsedSuccessful();
            } else {
                response->HttpCode = 404;
                return false;
            }
        }

        auto curl = curl_easy_init();
        struct curl_slist* curl_headers = nullptr;
        for (const auto& [key, value] : urlOptions.headers) {
            curl_headers = curl_slist_append(curl_headers, fmt::format("{}: {}", key, value).c_str());
        }

        auto escapedUrl = urlOptions.fullURl();

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
        curl_easy_setopt(curl, CURLOPT_URL, escapedUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, urlOptions.timeOut.value_or(timeOut));
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, urlOptions.encoding.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");

        if (progressReport != nullptr) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, false);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressReport);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](std::function<void(float)>* progressReport, curl_off_t dltotal, curl_off_t dlnow, curl_off_t utotal, curl_off_t unow){
                auto& func = *progressReport;
                // progress for get is the download values
                float progress = (float)dlnow / (float)dltotal;
                if (std::isnan(progress)) progress = 0.0f;
                func(progress);
                return 0;
            });
        }

        std::vector<uint8_t> recvData;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_vec_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &recvData);

        std::string recvHeaders;
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_str_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &recvHeaders);

        std::string userAgent = urlOptions.userAgent.value_or(this->userAgent);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, urlOptions.useSSL ? 1 : 0);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, urlOptions.useSSL ? 2 : 0);

        response->CurlStatus = curl_easy_perform(curl);

        int httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        response->HttpCode = httpCode;

        VERBOSE("Get result: curl {}, http {}", response->CurlStatus, httpCode);

        if (response->CurlStatus == CURLE_OK) {
            response->AcceptData(recvData);
            response->AcceptHeaders(recvHeaders);
        }

        curl_easy_cleanup(curl);
        return response->IsSuccessful() && response->DataParsedSuccessful();
    }

    bool DownloaderUtility::PostInto(URLOptions urlOptions, std::span<uint8_t const> data, IResponse* response, std::function<void(float)> progressReport) const {
        // if the url is for a filepath, write it to disk instead
        if (urlOptions.isFileURL()) {
            std::filesystem::path filePath(urlOptions.url.substr(7));
            if (response) response->CurlStatus = 0;

            if (filePath.has_filename()) {
                // if it exists, delete
                if (std::filesystem::exists(filePath)) {
                    std::filesystem::remove(filePath);
                }

                // write out
                std::ofstream file(filePath, std::ios::binary | std::ios::out);
                file.write((char*)data.data(), data.size());

                // response will just get 0 length return
                if (response) {
                    response->AcceptData(std::span<uint8_t, 0>());
                    return response->IsSuccessful() && response->DataParsedSuccessful();
                } else {
                    return true;
                }
            } else {
                if (response) response->HttpCode = 404;
                return false;
            }
        }

        auto curl = curl_easy_init();
        struct curl_slist* curl_headers = nullptr;
        for (const auto& [key, value] : urlOptions.headers) {
            curl_headers = curl_slist_append(curl_headers, fmt::format("{}: {}", key, value).c_str());
        }

        auto escapedUrl = urlOptions.fullURl();

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, curl_headers);
        curl_easy_setopt(curl, CURLOPT_URL, escapedUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, urlOptions.timeOut.value_or(timeOut));
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, urlOptions.encoding.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");

        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.size());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (char*)data.data());

        if (progressReport != nullptr) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, false);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressReport);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](std::function<void(float)>* progressReport, curl_off_t dltotal, curl_off_t dlnow, curl_off_t utotal, curl_off_t unow){
                auto& func = *progressReport;
                // progress for post is the upload values
                float progress = (float)unow/ (float)utotal;
                if (std::isnan(progress)) progress = 0.0f;
                func(progress);
                return 0;
            });
        }

        std::vector<uint8_t> recvData;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_vec_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &recvData);

        std::string recvHeaders;
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_str_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &recvHeaders);

        std::string userAgent = urlOptions.userAgent.value_or(this->userAgent);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, urlOptions.useSSL ? 1 : 0);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, urlOptions.useSSL ? 2 : 0);

        int curlStatus = curl_easy_perform(curl);

        int httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        VERBOSE("Post result: curl {}, http {}", curlStatus, httpCode);
        if (response) {
            response->CurlStatus = curlStatus;
            response->HttpCode = httpCode;

            if (response->CurlStatus == CURLE_OK) {
                response->AcceptData(recvData);
                response->AcceptHeaders(recvHeaders);
            }

            curl_easy_cleanup(curl);
            return response->IsSuccessful() && response->DataParsedSuccessful();
        }

        curl_easy_cleanup(curl);
        return curlStatus == CURLE_OK;
    }
}
