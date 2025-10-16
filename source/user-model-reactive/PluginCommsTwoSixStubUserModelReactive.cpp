
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

#include "PluginCommsTwoSixStubUserModelReactive.h"

#include <algorithm>
#include <chrono>

#include "JsonTypes.h"
#include "LinkUserModel.h"
#include "log.h"

PluginCommsTwoSixStubUserModel::PluginCommsTwoSixStubUserModel(IUserModelSdk *sdk) : sdk(sdk) {
    // No user input requests are needed, so user model is ready right away
    sdk->updateState(COMPONENT_STATE_STARTED);
}

ComponentStatus PluginCommsTwoSixStubUserModel::onUserInputReceived(RaceHandle handle,
                                                                    bool answered,
                                                                    const std::string &response) {
    TRACE_METHOD(handle, answered, response);
    // We don't make any user input requests
    return COMPONENT_OK;
}

UserModelProperties PluginCommsTwoSixStubUserModel::getUserModelProperties() {
    TRACE_METHOD();
    // TODO implement this
    return {};
}

std::shared_ptr<LinkUserModel> PluginCommsTwoSixStubUserModel::createLinkUserModel(
    const LinkID &linkId) {
    return std::make_shared<LinkUserModel>(linkId, nextActionId);
}

ComponentStatus PluginCommsTwoSixStubUserModel::addLink(const LinkID &link,
                                                        const LinkParameters & /* params */) {
    TRACE_METHOD(link);
    {
        std::lock_guard<std::mutex> lock(mutex);
        linkUserModels[link] = createLinkUserModel(link);
        addedLinks.insert(link);
    }
    sdk->onTimelineUpdated();
    return COMPONENT_OK;
}

ComponentStatus PluginCommsTwoSixStubUserModel::removeLink(const LinkID &link) {
    TRACE_METHOD(link);
    {
        std::lock_guard<std::mutex> lock(mutex);
        linkUserModels.erase(link);
    }
    sdk->onTimelineUpdated();
    return COMPONENT_OK;
}

ActionTimeline PluginCommsTwoSixStubUserModel::getTimeline(Timestamp start, Timestamp end) {
    TRACE_METHOD(start, end);
    std::lock_guard<std::mutex> lock(mutex);

    ActionTimeline timeline;
    for (auto &entry : linkUserModels) {
        auto linkTimeline = entry.second->getTimeline(start, end);
        timeline.insert(timeline.end(), linkTimeline.begin(), linkTimeline.end());
    }

    std::sort(timeline.begin(), timeline.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.timestamp == rhs.timestamp) {
            return lhs.actionId < rhs.actionId;
        }
        return lhs.timestamp < rhs.timestamp;
    });

    return timeline;
}
ComponentStatus PluginCommsTwoSixStubUserModel::onTransportEvent(const Event &event) {
    TRACE_METHOD(event.json);

    // NOTE: This is the ONLY time a new action is added to the link after its first action is added 
    // - thus the transport MUST create EXACTLY one event we understand each time it executes an action. 
    // If it fails to produce an event we will have no actions, and if it produces more than one event 
    // we will have more than one action, each of which can then become multiple actions creating an 
    // expanding frequency of actions.

    nlohmann::json eventJson = nlohmann::json::parse(event.json);
    logDebug("Received transport event: " + std::string(eventJson.dump()));

    double currentTime = []() -> double {
        std::chrono::duration<double> sinceEpoch =
        std::chrono::high_resolution_clock::now().time_since_epoch();
        return sinceEpoch.count();
    }();

    // EXAMPLE behavior: add a new Action to the Link this Event occurred on
    if (linkUserModels.find(eventJson["linkId"]) != linkUserModels.end()) {
        auto linkUserModel = linkUserModels.at(eventJson["linkId"]);
        linkUserModel->addAction(event.json, currentTime);
        sdk->onTimelineUpdated();
    } else {
        logError("Received transport event for unknown link: " + eventJson["linkId"].get<std::string>());
    }

    return COMPONENT_OK;
}


#ifndef TESTBUILD
IUserModelComponent *createUserModel(const std::string &usermodel, IUserModelSdk *sdk,
                                     const std::string &roleName,
                                     const PluginConfig &pluginConfig) {
    TRACE_FUNCTION(usermodel, roleName, pluginConfig.pluginDirectory);
    return new PluginCommsTwoSixStubUserModel(sdk);
}
void destroyUserModel(IUserModelComponent *component) {
    TRACE_FUNCTION();
    delete component;
}

const RaceVersionInfo raceVersion = RACE_VERSION;
#endif
