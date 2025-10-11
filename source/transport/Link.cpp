
//
// Copyright 2023 Two Six Technologies
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "Link.h"

#include <ITransportSdk.h>

#include <chrono>
#include <nlohmann/json.hpp>

#include "PersistentStorageHelpers.h"
#include "base64.h"
#include "curlwrap.h"
#include "log.h"

static const size_t ACTION_QUEUE_MAX_CAPACITY = 10;

namespace std {
static std::ostream &operator<<(std::ostream &out, const std::vector<RaceHandle> &handles) {
    return out << nlohmann::json(handles).dump();
}
}  // namespace std

Link::Link(LinkID linkId, LinkAddress address, LinkProperties properties, ITransportSdk *sdk) :
    sdk(sdk),
    linkId(std::move(linkId)),
    address(std::move(address)),
    properties(std::move(properties)) {
    this->properties.linkAddress = nlohmann::json(this->address).dump();
}

Link::~Link() {
    TRACE_METHOD(linkId);
    shutdown();
}

LinkID Link::getId() const {
    return linkId;
}

const LinkProperties &Link::getProperties() const {
    return properties;
}

/**
 * @brief Enqueues content associated with a specific action ID into the content queue.
 *
 * This method stores the provided content in the internal content queue, associating it
 * with the given action ID. The operation is thread-safe, ensuring that concurrent access
 * to the content queue is properly synchronized.
 *
 * @param actionId The unique identifier for the action to associate with the content.
 * @param content A vector of bytes representing the content to be enqueued.
 * @return COMPONENT_OK if the content is successfully enqueued.
 */
ComponentStatus Link::enqueueContent(uint64_t actionId, const std::vector<uint8_t> &content) {
    TRACE_METHOD(linkId, actionId);
    {
        std::lock_guard<std::mutex> lock(mutex);
        contentQueue[actionId] = content;
    }
    return COMPONENT_OK;
}

/**
 * @brief Removes content associated with the specified action ID from the content queue.
 *
 * This method locates the content in the queue using the provided action ID and removes it
 * if it exists. The operation is thread-safe as it uses a mutex to protect access to the queue.
 *
 * @param actionId The unique identifier for the action whose content is to be dequeued.
 * @return ComponentStatus Returns COMPONENT_OK to indicate successful operation.
 */
ComponentStatus Link::dequeueContent(uint64_t actionId) {
    TRACE_METHOD(linkId, actionId);
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto iter = contentQueue.find(actionId);
        if (iter != contentQueue.end()) {
            contentQueue.erase(iter);
        }
    }
    return COMPONENT_OK;
}

/**
 * @brief Fetches a new action for the link and adds it to the action queue.
 *
 * This method checks the state of the link and ensures that it is not shut down.
 * If the link is operational, it attempts to add a new action to the action queue.
 * If the action queue has reached its maximum capacity, an error is logged and
 * the method returns an error status.
 *
 * @return ComponentStatus 
 *         - COMPONENT_OK: If the action was successfully added to the queue.
 *         - COMPONENT_ERROR: If the link is shut down or the action queue is full.
 *
 * @note This method is thread-safe and uses a mutex to protect access to the
 *       action queue. It also notifies a condition variable after adding a new
 *       action to the queue.
 */
ComponentStatus Link::fetch() {
    TRACE_METHOD(linkId);

    if (isShutdown) {
        logError(logPrefix + "link has been shutdown: " + linkId);
        return COMPONENT_ERROR;
    }

    std::lock_guard<std::mutex> lock(mutex);

    if (actionQueue.size() >= ACTION_QUEUE_MAX_CAPACITY) {
        logError(logPrefix + "action queue full for link: " + linkId);
        return COMPONENT_ERROR;
    }

    actionQueue.push_back({false, {}, 0});
    conditionVariable.notify_one();
    return COMPONENT_OK;
}

/**
 * @brief Posts a set of race handles and an action ID to the link for processing.
 *
 * This method enqueues the provided race handles and action ID into the action queue
 * for further processing, provided the link is not shut down, the action queue is not
 * full, and there is enqueued content corresponding to the given action ID.
 *
 * @param handles A vector of RaceHandle objects to be processed.
 * @param actionId The unique identifier for the action associated with the handles.
 * @return ComponentStatus Returns COMPONENT_OK if the operation succeeds, or 
 *         COMPONENT_ERROR if the link is shut down, the action queue is full, or 
 *         there is no enqueued content for the given action ID.
 *
 * @note If the link is shut down, an error is logged, the package status is updated
 *       to PACKAGE_FAILED_GENERIC, and COMPONENT_ERROR is returned.
 * @note If the action queue is full, an error is logged, the package status is updated
 *       to PACKAGE_FAILED_GENERIC, and COMPONENT_ERROR is returned.
 * @note If there is no enqueued content for the given action ID, an informational log
 *       is generated, the package status is updated to PACKAGE_FAILED_GENERIC, and 
 *       COMPONENT_OK is returned.
 * @thread_safety This method is thread-safe as it uses a mutex to protect access to
 *                shared resources and a condition variable to notify waiting threads.
 */
ComponentStatus Link::post(std::vector<RaceHandle> handles, uint64_t actionId) {
    TRACE_METHOD(linkId, handles, actionId);

    if (isShutdown) {
        logError(logPrefix + "link has been shutdown: " + linkId);
        updatePackageStatus(handles, PACKAGE_FAILED_GENERIC);
        return COMPONENT_ERROR;
    }

    std::lock_guard<std::mutex> lock(mutex);

    if (actionQueue.size() >= ACTION_QUEUE_MAX_CAPACITY) {
        logError(logPrefix + "action queue full for link: " + linkId);
        updatePackageStatus(handles, PACKAGE_FAILED_GENERIC);
        return COMPONENT_ERROR;
    }

    if (contentQueue.find(actionId) == contentQueue.end()) {
        // TODO: what's the correct log level. We want it to be an error(?) for performer encodings,
        // but this is expected for our own comms plugin.
        logInfo(logPrefix + "no enqueued content for given action ID: " + std::to_string(actionId));
        updatePackageStatus(handles, PACKAGE_FAILED_GENERIC);
        return COMPONENT_OK;
    }

    actionQueue.push_back({true, std::move(handles), actionId});
    conditionVariable.notify_one();
    return COMPONENT_OK;
}

void Link::start() {
    TRACE_METHOD(linkId);
    thread = std::thread(&Link::runActionThread, this);
}

void Link::shutdown() {
    TRACE_METHOD(linkId);
    isShutdown = true;
    conditionVariable.notify_one();
    if (thread.joinable()) {
        thread.join();
    }
}

/**
 * @brief Executes the action thread for the Link object.
 * 
 * This method runs in a loop, processing actions from the action queue until
 * the Link object is shut down. It waits for new actions to be added to the
 * queue or for a shutdown signal. Depending on the type of action, it either
 * posts the action or fetches data.
 * 
 * @details
 * - The method uses a condition variable to wait for either a shutdown signal
 *   or the presence of actions in the queue.
 * - If a shutdown signal is received, the method logs the shutdown and exits
 *   the loop.
 * - If an action is available, it is dequeued and processed:
 *   - If the action is marked as `post`, the `postOnActionThread` method is
 *     called with the action's handles and ID.
 *   - Otherwise, the `fetchOnActionThread` method is called to fetch data,
 *     updating the `latest` index.
 * 
 * Thread Safety:
 * - The method uses a mutex to synchronize access to the action queue and
 *   ensure thread safety.
 * 
 * Preconditions:
 * - The `Link` object must be properly initialized before calling this method.
 * 
 * Postconditions:
 * - The method will terminate when the `isShutdown` flag is set to true.
 */
void Link::runActionThread() {
    TRACE_METHOD(linkId);
    logPrefix += linkId + ": ";

    int latest = getInitialIndex();

    while (not isShutdown) {
        std::unique_lock<std::mutex> lock(mutex);
        conditionVariable.wait(lock, [this] { return isShutdown or not actionQueue.empty(); });

        if (isShutdown) {
            logDebug(logPrefix + "shutting down");
            break;
        }

        auto action = actionQueue.front();
        actionQueue.pop_front();

        if (action.post) {
            postOnActionThread(action.handles, action.actionId);
        } else {
            latest = fetchOnActionThread(latest);
        }
    }
}

int Link::getInitialIndex() {
    TRACE_METHOD(linkId);
    logPrefix += linkId + ": ";

    // TODO check link hints for timestamp
    double timestamp = psh::readValue(sdk, prependIdentifier("lastTimestamp"), -1.0);
    if (timestamp > 0) {
        logDebug(logPrefix + "using last recorded timestamp: " + std::to_string(timestamp));
    } else if (address.timestamp <= 0) {
        std::chrono::duration<double> sinceEpoch =
            std::chrono::high_resolution_clock::now().time_since_epoch();
        timestamp = sinceEpoch.count();
        logDebug(logPrefix + "using now for timestamp: " + std::to_string(timestamp));
    } else {
        timestamp = address.timestamp;
        logDebug(logPrefix + "using address timestamp: " + std::to_string(timestamp));
    }

    return getIndexFromTimestamp(timestamp);
}

/**
 * @brief Fetches new posts from the server on the action thread.
 *
 * This method retrieves new posts starting from the specified latest index,
 * processes them, and updates the state accordingly. It handles various
 * exceptions that may occur during the fetch operation and retries if necessary.
 *
 * @param latestIndex The index of the latest post already fetched.
 * @return The new latest index after fetching posts, or the input latestIndex
 *         if the operation fails.
 *
 * @details
 * - The method logs errors and debug information during the fetch process.
 * - If fewer posts are received than expected, it logs a warning about potential
 *   data loss.
 * - Posts originating from the same source (self) are ignored.
 * - Successfully fetched posts are decoded, hashed, and passed to the SDK for
 *   further processing.
 * - The server timestamp is saved if new posts are fetched.
 * - If the fetch operation fails due to exceptions, it retries up to a maximum
 *   number of attempts. If the retry limit is reached, the component state is
 *   updated to indicate failure.
 *
 * @throws curl_exception If a network-related error occurs.
 * @throws nlohmann::json::exception If a JSON parsing error occurs.
 * @throws std::exception For other standard exceptions.
 */
int Link::fetchOnActionThread(int latestIndex) {
    TRACE_METHOD(linkId, latestIndex);
    logPrefix += linkId + ": ";

    try {
        auto [posts, newLatestIndex, serverTimestamp] = getNewPosts(latestIndex);

        int numPosts = static_cast<int>(posts.size());
        if (numPosts < newLatestIndex - latestIndex) {
            logError(logPrefix + "expected " + std::to_string(newLatestIndex - latestIndex) +
                     " posts, but only got " + std::to_string(numPosts) + ". " +
                     std::to_string(newLatestIndex - latestIndex - numPosts) +
                     " posts may have been lost.");
        }

        for (const auto &post : posts) {
            if (postedMessageHashes.findAndRemoveMessage(post)) {
                logDebug(logPrefix + "received post from self, ignoring");
            } else {
                logDebug(logPrefix + "received encrypted package");
                std::vector<uint8_t> message = base64::decode(post);
                logDebug("hash of fetch " + RaceLog::stringifyValues("hash", std::hash<std::string>()(std::string(message.begin(), message.end()))));
                sdk->onReceive(linkId, {linkId, "*/*", false, {}}, message);
            }
        }

        if (numPosts > 0) {
            psh::saveValue(sdk, prependIdentifier("lastTimestamp"), serverTimestamp);
        }

        fetchAttempts = 0;

        return newLatestIndex;
    } catch (curl_exception &error) {
        logError(logPrefix + "curl exception: " + std::string(error.what()));
    } catch (nlohmann::json::exception &error) {
        logError(logPrefix + "json exception: " + std::string(error.what()));
    } catch (std::exception &error) {
        logError(logPrefix + "std exception: " + std::string(error.what()));
    }

    ++fetchAttempts;
    if (fetchAttempts >= address.maxTries) {
        logError(logPrefix + "Retry limit reached. Giving up.");
        sdk->updateState(COMPONENT_STATE_FAILED);
    }

    return latestIndex;
}

/**
 * @brief callback function required by libcurl-dev.
 * See documentation in link below:
 * https://curl.haxx.se/libcurl/c/libcurl-tutorial.html
 */
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    (static_cast<std::string *>(userp))->append(static_cast<char *>(contents), size * nmemb);
    return size * nmemb;
}

/**
 * @brief Retrieves the index of a post based on a given timestamp.
 *
 * This method constructs a URL using the provided timestamp and sends an HTTP GET request
 * to retrieve the index of a post that matches the timestamp. The response is expected
 * to be in JSON format containing an "index" field. If an error occurs during the HTTP
 * request or JSON parsing, the method logs the error and returns 0.
 *
 * @param secondsSinceEpoch The timestamp in seconds since the Unix epoch.
 * @return The index of the post if successful, or 0 if an error occurs.
 *
 * @throws curl_exception If an error occurs during the HTTP request.
 * @throws nlohmann::json::exception If an error occurs during JSON parsing.
 * @throws std::exception For any other unexpected errors.
 */
int Link::getIndexFromTimestamp(double secondsSinceEpoch) {
    TRACE_METHOD(linkId, secondsSinceEpoch);
    logPrefix += linkId + ": ";

    std::string url = "http://" + address.hostname + ":" + std::to_string(address.port) +
                      "/after/" + address.hashtag + "/" + std::to_string(secondsSinceEpoch);

    // return 0 if error
    int index = 0;
    try {
        CurlWrap curl;
        std::string response;

        logDebug(logPrefix + "Attempting to get post by timestamp from: " + url);

        curl.setopt(CURLOPT_URL, url.c_str());
        curl.setopt(CURLOPT_WRITEFUNCTION, WriteCallback);
        curl.setopt(CURLOPT_WRITEDATA, &response);
        curl.perform();

        index = nlohmann::json::parse(response).at("index").get<int>();
        logDebug(logPrefix + "Got index: " + std::to_string(index));

    } catch (curl_exception &error) {
        logError(logPrefix + "curl exception: " + std::string(error.what()));
    } catch (nlohmann::json::exception &error) {
        logError(logPrefix + "json exception: " + std::string(error.what()));
    } catch (std::exception &error) {
        logError(logPrefix + "std exception: " + std::string(error.what()));
    }

    return index;
}

/**
 * @brief Fetches new posts from a remote server starting from the specified index.
 * 
 * This method constructs a URL based on the provided latest index and the Link's
 * address details, sends an HTTP GET request to the server, and parses the response
 * JSON to extract the data, length, and timestamp of the posts.
 * 
 * @param latestIndex The index of the latest post already retrieved. Posts starting
 *                    from this index (inclusive) will be fetched.
 * @return A tuple containing:
 *         - A vector of strings representing the new posts.
 *         - An integer representing the length of the data.
 *         - A double representing the timestamp of the latest post.
 * 
 * @throws nlohmann::json::exception If the response JSON is malformed or missing
 *                                   required fields.
 * @throws std::exception If the HTTP request fails or other errors occur during
 *                        processing.
 */
std::tuple<std::vector<std::string>, int, double> Link::getNewPosts(int latestIndex) {
    TRACE_METHOD(linkId, latestIndex);
    logPrefix += linkId + ": ";

    // Get all posts after (and including) oldest
    std::string url = "http://" + address.hostname + ":" + std::to_string(address.port) + "/get/" +
                      address.hashtag + "/" + std::to_string(latestIndex) + "/-1";

    CurlWrap curl;
    std::string response;

    curl.setopt(CURLOPT_URL, url.c_str());
    curl.setopt(CURLOPT_WRITEFUNCTION, WriteCallback);
    curl.setopt(CURLOPT_WRITEDATA, &response);
    curl.perform();

    auto responseJson = nlohmann::json::parse(response);

    return {responseJson.at("data"), responseJson.at("length"),
            std::stod(responseJson.at("timestamp").get<std::string>())};
}

/**
 * @brief Posts a message on the action thread for the given handles and action ID.
 *
 * This method retrieves the content associated with the provided action ID from the content queue,
 * encodes it in base64, and attempts to post it to the whiteboard. If the post fails after the
 * maximum number of retries, it logs an error and updates the package status to indicate failure.
 * Otherwise, it updates the package status to indicate success.
 *
 * @param handles A vector of RaceHandle objects associated with the action.
 * @param actionId The unique identifier for the action to be posted.
 *
 * @details
 * - Logs an error and updates the package status to `PACKAGE_FAILED_GENERIC` if the action ID is
 *   not found in the content queue.
 * - Encodes the content in base64 and calculates a hash for logging purposes.
 * - Attempts to post the encoded message to the whiteboard up to `address.maxTries` times.
 * - Removes the message hash and updates the package status to `PACKAGE_FAILED_GENERIC` if all
 *   retries fail.
 * - Updates the package status to `PACKAGE_SENT` if the post succeeds.
 */
void Link::postOnActionThread(const std::vector<RaceHandle> &handles, uint64_t actionId) {
    TRACE_METHOD(linkId, handles, actionId);
    logPrefix += linkId + ": ";

    auto iter = contentQueue.find(actionId);
    if (iter == contentQueue.end()) {
        // We really shouldn't get here, since we already check for this before queueing the action,
        // but just in case...
        logError(logPrefix +
                 "no enqueued content for given action ID: " + std::to_string(actionId));
        updatePackageStatus(handles, PACKAGE_FAILED_GENERIC);
        return;
    }

    logDebug("hash of post " + RaceLog::stringifyValues("hash", std::hash<std::string>()(std::string(iter->second.begin(), iter->second.end()))));
    std::string message = base64::encode(iter->second);
    auto msgHash = postedMessageHashes.addMessage(message);

    int tries = 0;
    for (; tries < address.maxTries; ++tries) {
        if (postToWhiteboard(message)) {
            break;
        }
    }

    if (tries == address.maxTries) {
        logError(logPrefix + "retry limit exceeded: post failed");
        postedMessageHashes.removeHash(msgHash);
        updatePackageStatus(handles, PACKAGE_FAILED_GENERIC);
    } else {
        updatePackageStatus(handles, PACKAGE_SENT);
    }
}

/**
 * @brief Updates the status of a package for a list of race handles.
 * 
 * This method iterates through the provided list of race handles and updates
 * the package status for each handle by invoking the `onPackageStatusChanged`
 * method on the SDK instance.
 * 
 * @param handles A vector of RaceHandle objects representing the race handles
 *                whose package statuses need to be updated.
 * @param status  The new status to be applied to the packages associated with
 *                the provided race handles.
 */
void Link::updatePackageStatus(const std::vector<RaceHandle> &handles, PackageStatus status) {
    for (auto &handle : handles) {
        sdk->onPackageStatusChanged(handle, status);
    }
}

/**
 * @brief Posts a message to a whiteboard server using an HTTP POST request.
 * 
 * This method constructs a JSON payload containing the provided message and sends it
 * to a specified URL using libcurl. The URL is constructed based on the `address` member
 * of the `Link` object. The method logs the process and handles potential exceptions.
 * 
 * @param message The message to be posted to the whiteboard server.
 * @return true if the post operation is successful and the response contains "index",
 *         false otherwise.
 * 
 * @note The method uses a custom `CurlWrap` wrapper for libcurl operations and assumes
 *       the existence of a `WriteCallback` function for handling response data.
 * 
 * @warning The `headers` object is manually managed and freed at the end of the method.
 *          Consider refactoring to use RAII for better resource management.
 * 
 * @throws curl_exception if an error occurs during the libcurl operation.
 */
bool Link::postToWhiteboard(const std::string &message) {
    TRACE_METHOD(linkId);
    logPrefix += linkId + ": ";
    bool success = false;

    std::string url = "http://" + address.hostname + ":" + std::to_string(address.port) + "/post/" +
                      address.hashtag;

    std::string postData;
    postData.reserve(message.size() + 14);
    postData.append("{ \"data\":\"");
    postData.append(message);
    postData.append("\" }");

    // TODO: RAII this thing
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    try {
        CurlWrap curl;
        std::string response;

        logDebug(logPrefix + "Attempting to post to: " + url);

        curl.setopt(CURLOPT_URL, url.c_str());
        curl.setopt(CURLOPT_HTTPPOST, 1L);

        // connecton timeout. override the default and set to 10 seconds.
        curl.setopt(CURLOPT_CONNECTTIMEOUT, 10L);

        curl.setopt(CURLOPT_WRITEFUNCTION, WriteCallback);
        curl.setopt(CURLOPT_WRITEDATA, &response);
        curl.setopt(CURLOPT_HTTPHEADER, headers);
        curl.setopt(CURLOPT_POSTFIELDS, postData.c_str());
        curl.setopt(CURLOPT_POSTFIELDSIZE, postData.size());

        curl.perform();

        if (response.find("index") != std::string::npos) {
            logDebug(logPrefix + "Post successful: " + response);
            success = true;
        } else {
            logWarning(logPrefix + "Unknown reponse: " + response);
        }
    } catch (curl_exception &error) {
        logWarning(logPrefix + "curl exception: " + std::string(error.what()));
    }

    curl_slist_free_all(headers);

    return success;
}

/**
 * @brief Prepends an identifier to the given key by combining it with the 
 *        hostname, port, and hashtag of the Link's address.
 * 
 * @param key The input string to which the identifier will be prepended.
 * @return A string in the format: key:hostname:port:hashtag.
 */
std::string Link::prependIdentifier(const std::string &key) {
    return key + ":" + address.hostname + ":" + std::to_string(address.port) + ":" +
           address.hashtag;
}
