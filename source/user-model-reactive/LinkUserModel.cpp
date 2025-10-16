
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

#include "LinkUserModel.h"

#include "JsonTypes.h"
#include "log.h"

static const double WAIT_TIME = 5.0;

LinkUserModel::LinkUserModel(const LinkID &linkId, std::atomic<uint64_t> &nextActionId) :
    linkId(linkId), nextActionId(nextActionId) {}

MarkovModel::UserAction LinkUserModel::getNextUserAction() {
    return model.getNextUserAction();
}

ActionType LinkUserModel::convertUserActionToActionType(MarkovModel::UserAction userAction) {
    switch (userAction) {
        case MarkovModel::UserAction::FETCH:
            return ACTION_FETCH;
        case MarkovModel::UserAction::POST:
            return ACTION_POST;
        default:
            return ACTION_UNDEF;
    }
}

void LinkUserModel::addAction(const std::string &eventString, Timestamp timestamp) {
    auto actTime = timestamp + WAIT_TIME;
    
    auto eventJson = nlohmann::json::parse(eventString); // Could parameterize model.getNextUserAction() based on event contents
    auto action = getNextUserAction();
    while (action == MarkovModel::UserAction::WAIT) {
            actTime += WAIT_TIME;
            action = getNextUserAction();
    } 
    nlohmann::json actionJson = ActionJson{
            linkId,
            convertUserActionToActionType(action),
    };
    cachedTimeline.push_back({
            actTime,
            ++nextActionId,
            actionJson.dump(),
    });
    logDebug("LinkUserModel " + linkId + " added action at " +
             std::to_string(actTime) + " with " + std::to_string(cachedTimeline.size()) + " cached actions");
    for (const auto &a : cachedTimeline) {
        logDebug("  Action at " + std::to_string(a.timestamp) + ": " + a.json);
    }
}

ActionTimeline LinkUserModel::getTimeline(Timestamp start, Timestamp end) {
    // First, remove all actions from the cached timeline that occur before the `start` time
    auto iter = std::find_if(cachedTimeline.begin(), cachedTimeline.end(),
                             [start](const auto &val) { return val.timestamp >= start; });
    cachedTimeline.erase(cachedTimeline.begin(), iter);

    // Insert an action at start + WAIT_TIME if this is the first time getTimeline() is called to kick off the chain
    if (firstGetTimeline) {
        logDebug("LinkUserModel " + linkId + " generating timeline from " +
                 std::to_string(start) + " to " + std::to_string(end) +
                 " for initial getTimeline()");
        addAction("{}", start);
        firstGetTimeline = false;
    }
    logDebug("LinkUserModel " + linkId + " generating timeline from " +
             std::to_string(start) + " to " + std::to_string(end) +
             " with " + std::to_string(cachedTimeline.size()) + " actions");
    for (const auto &a : cachedTimeline) {
        logDebug("  Action at " + std::to_string(a.timestamp) + ": " + a.json);
    }
    return cachedTimeline;
}