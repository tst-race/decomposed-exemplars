
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

#include "MessageHashQueue.h"

#include <algorithm>
#include <string>

/**
 * @brief Computes a hash value for the given message string.
 *
 * This function takes a string message as input and computes a hash value
 * using the standard library's std::hash for strings. The hash value can
 * be used for efficient storage and retrieval in hash-based data structures.
 *
 * @param message The input string for which the hash value is to be computed.
 * @return A std::size_t representing the hash value of the input message.
 */
std::size_t MessageHashQueue::hash(const std::string &message) {
    return std::hash<std::string>()({message.begin(), message.end()});
}

/**
 * @brief Adds a hashed message to the queue. If the queue exceeds the maximum
 *        size, the oldest message hash is removed to make space.
 * 
 * @param message The input message to be hashed and added to the queue.
 * @return The hash of the input message.
 */
std::size_t MessageHashQueue::addMessage(const std::string &message) {
    if (queue.size() > max) {
        queue.pop_front();
    }
    auto msgHash = hash(message);
    queue.push_back(msgHash);
    return msgHash;
}

/**
 * @brief Removes a specific hash from the message queue if it exists.
 *
 * This function searches for the given hash in the queue. If the hash is found,
 * it is removed from the queue. If the hash is not found, the function does nothing.
 *
 * @param hash The hash value to be removed from the queue.
 */
void MessageHashQueue::removeHash(std::size_t hash) {
    auto iter = std::find(queue.begin(), queue.end(), hash);
    if (iter != queue.end()) {
        queue.erase(iter);
    }
}

/**
 * @brief Searches for a hashed message in the queue and removes it along with all preceding elements.
 * 
 * This function takes a message, hashes it, and searches for the corresponding hash in the queue.
 * If the hash is found, it removes the element and all elements before it in the queue.
 * 
 * @param message The message to search for in the queue.
 * @return true if the message hash was found and removed; false otherwise.
 */
bool MessageHashQueue::findAndRemoveMessage(const std::string &message) {
    auto iter = std::find(queue.begin(), queue.end(), hash(message));
    bool found = iter != queue.end();
    if (found) {
        queue.erase(queue.begin(), iter + 1);
    }
    return found;
}