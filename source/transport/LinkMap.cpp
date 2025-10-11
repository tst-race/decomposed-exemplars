
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

#include "LinkMap.h"

/**
 * @brief Retrieves the number of links in the LinkMap.
 * 
 * This method provides a thread-safe way to access the size of the
 * internal links container by using a mutex to ensure synchronization.
 * 
 * @return The number of links currently stored in the LinkMap.
 */
int LinkMap::size() const {
    std::lock_guard<std::mutex> lock(mutex);
    return links.size();
}

/**
 * @brief Clears all links from the LinkMap.
 * 
 * This function removes all elements from the internal links container,
 * ensuring that the operation is thread-safe by using a mutex lock.
 */
void LinkMap::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    links.clear();
}

/**
 * @brief Adds a new link to the LinkMap.
 * 
 * This method safely adds a shared pointer to a Link object into the internal
 * map of links, using the link's unique identifier as the key. Thread safety
 * is ensured by locking a mutex during the operation.
 * 
 * @param link A shared pointer to the Link object to be added. The Link object
 *             must have a valid unique identifier accessible via getId().
 */
void LinkMap::add(const std::shared_ptr<Link> &link) {
    std::lock_guard<std::mutex> lock(mutex);
    links[link->getId()] = link;
}

/**
 * @brief Retrieves a shared pointer to a Link object associated with the given LinkID.
 * 
 * This method provides thread-safe access to the internal map of links. It locks
 * the mutex to ensure that no other thread can modify the map while the lookup
 * is being performed. If the specified LinkID does not exist in the map, this
 * method will throw a `std::out_of_range` exception.
 * 
 * @param linkId The identifier of the Link to retrieve.
 * @return std::shared_ptr<Link> A shared pointer to the Link object associated with the given LinkID.
 * @throws std::out_of_range If the specified LinkID is not found in the map.
 */
std::shared_ptr<Link> LinkMap::get(const LinkID &linkId) const {
    std::lock_guard<std::mutex> lock(mutex);
    return links.at(linkId);
}

/**
 * @brief Retrieves a copy of the internal map of links.
 * 
 * This method provides thread-safe access to the internal unordered map
 * of links by locking a mutex before returning a copy of the map. The
 * returned map contains associations between LinkID and shared pointers
 * to Link objects.
 * 
 * @return std::unordered_map<LinkID, std::shared_ptr<Link>> 
 *         A copy of the internal map of links.
 */
std::unordered_map<LinkID, std::shared_ptr<Link>> LinkMap::getMap() const {
    std::lock_guard<std::mutex> lock(mutex);
    return links;
}

/**
 * @brief Removes a link from the map by its ID.
 *
 * This function removes the link associated with the given LinkID from the
 * internal map. If the link is found, it is erased from the map and a shared
 * pointer to the removed link is returned. If the link is not found, a null
 * shared pointer is returned.
 *
 * @param linkId The ID of the link to be removed.
 * @return std::shared_ptr<Link> A shared pointer to the removed link if it
 *         exists, or a null shared pointer if the link was not found.
 */
std::shared_ptr<Link> LinkMap::remove(const LinkID &linkId) {
    std::lock_guard<std::mutex> lock(mutex);
    std::shared_ptr<Link> value;
    auto iter = links.find(linkId);
    if (iter != links.end()) {
        value = iter->second;
        links.erase(iter);
    }
    return value;
}