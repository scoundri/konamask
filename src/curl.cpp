#include "curl.h"
#include <iostream>
#include <fstream>
#include <curl/curl.h>

size_t WriteData(void* ptr, size_t size, size_t nmemb, void* stream) {
    std::ofstream* out = static_cast<std::ofstream*>(stream);
    size_t totalSize = size * nmemb;
    out->write(static_cast<char*>(ptr), totalSize);
    return totalSize;
}

bool Fetch::Download(const char* url, const char* output) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cout << "[ERROR] (CURL) Failed to initialize CURL." << std::endl;
        return false;
    }

    std::ofstream file(output, std::ios::binary);
    if (!file.is_open()) {
        std::cout << "[ERROR] (CURL) Failed to open output file." << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }

    // URL and write callback
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    // timeout
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L); // max time to establish connection
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);       // max total time for request

    // perform request
    CURLcode res = curl_easy_perform(curl);

    long response_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    }

    curl_easy_cleanup(curl);
    file.close();

    // on any failure, delete what was written
    if (res != CURLE_OK || response_code != 200) {
        // remove zero-length or partial file
        std::remove(output);

        if (res != CURLE_OK) {
            std::cout << "[ERROR] (CURL) Error: " << curl_easy_strerror(res) << std::endl;
        } else {
            std::cout << "[ERROR] (CURL) Fetch error: status code " << response_code << std::endl;
        }
        return false;
    }

    return true;
}
