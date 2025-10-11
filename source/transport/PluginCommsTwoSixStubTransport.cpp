
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

#include "PluginCommsTwoSixStubTransport.h"

#include <chrono>
#include <nlohmann/json.hpp>

#include "JsonTypes.h"
#include "Link.h"
#include "LinkAddress.h"
#include "log.h"

namespace std {
static std::ostream &operator<<(std::ostream &out, const std::vector<RaceHandle> &handles) {
    return out << nlohmann::json(handles).dump();
}
}  // namespace std


/**
 * @brief Creates a default set of link properties based on the provided channel properties.
 *
 * This function initializes a LinkProperties object using the values from the given
 * ChannelProperties object.
 *
 * @param channelProperties The channel properties used to initialize the link properties.
 * @return A LinkProperties object initialized with default values derived from the channel properties.
 */
LinkProperties createDefaultLinkProperties(const ChannelProperties &channelProperties) {
    LinkProperties linkProperties;

    linkProperties.linkType = LT_BIDI;
    linkProperties.transmissionType = channelProperties.transmissionType;
    linkProperties.connectionType = channelProperties.connectionType;
    linkProperties.sendType = channelProperties.sendType;
    linkProperties.reliable = channelProperties.reliable;
    linkProperties.isFlushable = channelProperties.isFlushable;
    linkProperties.duration_s = channelProperties.duration_s;
    linkProperties.period_s = channelProperties.period_s;
    linkProperties.mtu = channelProperties.mtu;

    linkProperties.worst = channelProperties.creatorExpected;
    linkProperties.expected = channelProperties.creatorExpected;
    linkProperties.best = channelProperties.creatorExpected;

    linkProperties.supported_hints = channelProperties.supportedHints;
    linkProperties.channelGid = channelProperties.channelGid;

    return linkProperties;
}

/**
 * @brief Constructs a PluginCommsTwoSixStubTransport object.
 *
 * This constructor initializes the transport with the provided SDK interface,
 * retrieves the active persona, channel properties, and creates default link
 * properties based on the channel properties. It also sets the transport state
 * to COMPONENT_STATE_STARTED, indicating that the transport is ready for use
 * without requiring any user input.
 *
 * @param sdk Pointer to the ITransportSdk interface used for communication and
 *            retrieving necessary transport-related properties.
 */
PluginCommsTwoSixStubTransport::PluginCommsTwoSixStubTransport(ITransportSdk *sdk) :
    sdk(sdk),
    racePersona(sdk->getActivePersona()),
    channelProperties(sdk->getChannelProperties()),
    defaultLinkProperties(createDefaultLinkProperties(channelProperties)) {
    // No user input requests are needed, so transport is ready right away
    sdk->updateState(COMPONENT_STATE_STARTED);
}

/**
 * Handles user input received for a specific race handle. 
 *
 * @param handle The race handle associated with the user input.
 * @param answered A boolean indicating whether the user input was answered.
 * @param response The string containing the user's response.
 * @return COMPONENT_OK to indicate successful handling of the user input.
 *
 * Note: This transport component does not make any user input requests.
 */
ComponentStatus PluginCommsTwoSixStubTransport::onUserInputReceived(RaceHandle handle,
                                                                    bool answered,
                                                                    const std::string &response) {
    TRACE_METHOD(handle, answered, response);
    // We don't make any user input requests
    return COMPONENT_OK;
}

/**
 * Retrieves the transport properties for the PluginCommsTwoSixStubTransport.
 *
 * @return A TransportProperties object containing the supported actions and their associated properties.
 *         - Supported actions:
 *           - "post": Accepts all MIME types for encoded data, meaning any encoder can be composed with it.
 *           - "fetch": Retrieves data, dispatch data to encoders based on examination of MIME type.
 */
TransportProperties PluginCommsTwoSixStubTransport::getTransportProperties() {
    TRACE_METHOD();
    return {
        // supportedActions
        {
            {"post", {"*/*"}},
            {"fetch", {}},
        },
    };
}

/**
 * Retrieves the properties of a specific link identified by the given LinkID.
 *
 * @param linkId The identifier of the link whose properties are to be retrieved.
 * @return A LinkProperties object containing the properties of the specified link.
 * @throws std::exception If the linkId does not exist or the properties cannot be retrieved.
 */
LinkProperties PluginCommsTwoSixStubTransport::getLinkProperties(const LinkID &linkId) {
    TRACE_METHOD(linkId);
    return links.get(linkId)->getProperties();
}

/**
 * @brief Validates the conditions for creating a new link in the transport layer.
 *
 * This method checks whether the creation of a new link is permissible based on
 * the current channel properties and the provided parameters. If the conditions
 * are not met, it logs an error and notifies the SDK about the link destruction.
 *
 * @param logPrefix A prefix string used for logging messages.
 * @param handle The race handle associated with the operation for use in link status callbacks.
 * @param linkId The unique identifier of the link to be created.
 * @param invalideRoleLinkSide The link side that is considered invalid for the current role (if any).
 * @return `true` if the link creation is allowed, `false` otherwise.
 *
 * @details
 * - The method checks if the number of existing links exceeds the maximum allowed links.
 * - It verifies whether the current role's link side is undefined or matches the invalid link side.
 * - If any condition fails, an error is logged, the SDK is notified that the link is destroyed (to avoid amiguity),
 *   and the method returns `false`.
 */
bool PluginCommsTwoSixStubTransport::preLinkCreate(const std::string &logPrefix, RaceHandle handle,
                                                   const LinkID &linkId,
                                                   LinkSide invalideRoleLinkSide) {
    int numLinks = links.size();
    if (channelProperties.maxLinks > 0 && numLinks >= channelProperties.maxLinks) {
        logError(logPrefix + "preLinkCreate: Too many links. links: " + std::to_string(numLinks) +
                 ", maxLinks: " + std::to_string(channelProperties.maxLinks));
        sdk->onLinkStatusChanged(handle, linkId, LINK_DESTROYED, {});
        return false;
    }

    if (channelProperties.currentRole.linkSide == LS_UNDEF ||
        channelProperties.currentRole.linkSide == invalideRoleLinkSide) {
        logError(logPrefix + "preLinkCreate: Invalid role for this call. currentRole: '" +
                 channelProperties.currentRole.roleName +
                 "' linkSide: " + linkSideToString(channelProperties.currentRole.linkSide));
        sdk->onLinkStatusChanged(handle, linkId, LINK_DESTROYED, {});
        return false;
    }

    return true;
}

/**
 * Handles the creation of a link after it has been established.
 *
 * @param logPrefix A string prefix used for logging messages.
 * @param handle The race handle associated with the link for link status callbacks.
 * @param linkId The unique identifier for the link.
 * @param link A shared pointer to the Link object. If null, the link is considered invalid.
 * @param linkStatus The status of the link to be reported.
 * @return COMPONENT_OK if the link is successfully added and status updated; 
 *         COMPONENT_ERROR if the link is null or an error occurs.
 *
 * This function performs the following actions:
 * - Logs an error and reports the link as destroyed if the provided link is null.
 * - Adds the link to the internal collection of links.
 * - Notifies the SDK of the link's status change.
 */
ComponentStatus PluginCommsTwoSixStubTransport::postLinkCreate(const std::string &logPrefix,
                                                               RaceHandle handle,
                                                               const LinkID &linkId,
                                                               const std::shared_ptr<Link> &link,
                                                               LinkStatus linkStatus) {
    if (link == nullptr) {
        logError(logPrefix + "postLinkCreate: link was null");
        sdk->onLinkStatusChanged(handle, linkId, LINK_DESTROYED, {});
        return COMPONENT_ERROR;
    }

    links.add(link);
    sdk->onLinkStatusChanged(handle, linkId, linkStatus, {});

    return COMPONENT_OK;
}

/**
 * @brief Creates and initializes a new instance of a Link object.
 *
 * This method constructs a shared pointer to a Link object using the provided
 * link ID, address, and properties, along with the SDK instance. After the Link
 * object is created, start() is called to begin link operation.
 *
 * @param linkId The unique identifier for the link.
 * @param address The address associated with the link.
 * @param properties The properties defining the link's configuration.
 * @return A shared pointer to the newly created and started Link instance.
 */
std::shared_ptr<Link> PluginCommsTwoSixStubTransport::createLinkInstance(
    const LinkID &linkId, const LinkAddress &address, const LinkProperties &properties) {
    auto link = std::make_shared<Link>(linkId, address, properties, sdk);
    link->start();
    return link;
}

/**
 * @brief CREATE a communication link with the specified handle and link ID.
 *
 * This method initializes a new communication link by generating a unique
 * address and applying default link properties. It performs pre-creation
 * checks and invokes post-creation logic to finalize the link setup.
 *
 * NOTE: this method GENERATES a new link address, parameterized by class variables, but not concretely specified (see createLinkFromAddress() for that functionality).
 *
 * @param handle The race handle of the createLink call, used for link status callbacks.
 * @param linkId The unique identifier for the link to be created.
 * @return ComponentStatus Returns COMPONENT_ERROR if pre-creation checks fail,
 *         otherwise returns the status of the link creation process.
 *
 * The link address is constructed using a hashtag that combines a prefix,
 * the race persona, and a unique numeric identifier. The timestamp is
 * derived from the current time since epoch in seconds.
 */
ComponentStatus PluginCommsTwoSixStubTransport::createLink(RaceHandle handle,
                                                           const LinkID &linkId) {
    TRACE_METHOD(handle, linkId);
    if (not preLinkCreate(logPrefix, handle, linkId, LS_LOADER)) {
        return COMPONENT_ERROR;
    }

    LinkAddress address;
    address.hashtag = "cpp_" + racePersona + "_" + std::to_string(nextAvailableHashTag++);
    std::chrono::duration<double> sinceEpoch =
        std::chrono::high_resolution_clock::now().time_since_epoch();
    address.timestamp = sinceEpoch.count();

    LinkProperties properties = defaultLinkProperties;

    auto link = createLinkInstance(linkId, address, properties);

    return postLinkCreate(logPrefix, handle, linkId, link, LINK_CREATED);
}

/**
 * @brief LOAD a link address and instantiates a link instance for that address with default properties.
 *
 * This method parses the provided link address, initializes the link properties
 * with default values, and instantiates a link instance. It also performs pre-creation
 * and post-creation operations to ensure the link is properly set up.
 *
 * @param handle The race handle used for link status callbacks.
 * @param linkId The unique identifier for the link being created.
 * @param linkAddress The serialized link address in JSON format.
 * @return ComponentStatus Returns COMPONENT_OK if the operation is successful.
 */
ComponentStatus PluginCommsTwoSixStubTransport::loadLinkAddress(RaceHandle handle,
                                                                const LinkID &linkId,
                                                                const std::string &linkAddress) {
    TRACE_METHOD(handle, linkId, linkAddress);
    if (not preLinkCreate(logPrefix, handle, linkId, LS_CREATOR)) {
        return COMPONENT_OK;
    }

    LinkAddress address = nlohmann::json::parse(linkAddress);
    LinkProperties properties = defaultLinkProperties;
    auto link = createLinkInstance(linkId, address, properties);

    return postLinkCreate(logPrefix, handle, linkId, link, LINK_LOADED);
}

/**
 * @brief LOADS a set of link addresses to instantiate a new link.
 * 
 * This method is a stub implementation because this transport does not support multi-address loading.
 * It triggers a link status change to LINK_DESTROYED and returns COMPONENT_ERROR.
 * 
 * @param handle The race handle associated with the operation, used for link status callbacks.
 * @param linkId The identifier of the link for which addresses are being loaded.
 * @param linkAddresses A vector of link addresses (unused in this implementation).
 * @return ComponentStatus Returns COMPONENT_ERROR to indicate failure.
 */
ComponentStatus PluginCommsTwoSixStubTransport::loadLinkAddresses(
    RaceHandle handle, const LinkID &linkId, const std::vector<std::string> & /* linkAddresses */) {
    TRACE_METHOD(handle, linkId);

    // We do not support multi-address loading
    sdk->onLinkStatusChanged(handle, linkId, LINK_DESTROYED, {});
    return COMPONENT_ERROR;
}

/**
 * @brief CREATE a link with the provided address and initializes it with default properties.
 * 
 * This method parses the given link address, instantiates a link instance, and performs
 * pre- and post-link creation operations. It ensures the link is properly set up
 * and ready for use.
 * NOTE: This method is used to create a link from an existing JSON link address, to generate a new link with a dynamically created address, see createLink()
 * 
 * @param handle The race handle associated with the operation, used for link status callbacks.
 * @param linkId The unique identifier for the link to be created.
 * @param linkAddress The address of the link in stringified JSON format.
 * @return ComponentStatus Returns COMPONENT_OK if the link creation is successful.
 * 
 * @note The method uses preLinkCreate and postLinkCreate to handle setup and cleanup
 *       operations around the link creation process.
 */
ComponentStatus PluginCommsTwoSixStubTransport::createLinkFromAddress(
    RaceHandle handle, const LinkID &linkId, const std::string &linkAddress) {
    TRACE_METHOD(handle, linkId, linkAddress);
    if (not preLinkCreate(logPrefix, handle, linkId, LS_LOADER)) {
        return COMPONENT_OK;
    }

    LinkAddress address = nlohmann::json::parse(linkAddress);
    LinkProperties properties = defaultLinkProperties;
    auto link = createLinkInstance(linkId, address, properties);

    return postLinkCreate(logPrefix, handle, linkId, link, LINK_CREATED);
}

/**
 * @brief Destroys a communication link identified by the given LinkID.
 *
 * This method removes the specified link from the internal collection of links
 * and shuts it down if it exists. If the link does not exist, an error is logged
 * and an error status is returned.
 *
 * @param handle The RaceHandle associated with the operation.
 * @param linkId The unique identifier of the link to be destroyed.
 * @return COMPONENT_OK if the link was successfully destroyed, 
 *         COMPONENT_ERROR if the link does not exist.
 */
ComponentStatus PluginCommsTwoSixStubTransport::destroyLink(RaceHandle handle,
                                                            const LinkID &linkId) {
    TRACE_METHOD(handle, linkId);

    auto link = links.remove(linkId);
    if (not link) {
        logError(logPrefix + "link with ID '" + linkId + "' does not exist");
        return COMPONENT_ERROR;
    }

    link->shutdown();

    return COMPONENT_OK;
}

/**
 * Retrieves the encoding parameters for a given action.
 *
 * This method parses the JSON representation of the action and determines
 * the appropriate encoding parameters based on the action type. If the action
 * type is unrecognized or if there is an error in parsing the JSON, the method
 * logs an error, updates the component state to failed, and returns an empty
 * vector.
 * 
 * Otherwise, it returns a vector of EncodingParameters including the link ID, the MIME type(s) the action can be provided by an encoder, whether the action can encode message data, and any additional JSON data associated with the action.
 *
 * @param action The action object containing the action ID and JSON data.
 * @return A vector of EncodingParameters corresponding to the action type.
 *         Returns an empty vector if the action type is ACTION_FETCH or if
 *         an error occurs.
 *
 * Possible action types:
 * - ACTION_FETCH: Returns an empty vector.
 * - ACTION_POST: Returns a vector with encoding parameters including the link ID,
 *   content type, and other relevant details.
 *
 * Error Handling:
 * - Logs an error if the action type is unrecognized.
 * - Logs an error if there is an issue parsing the action JSON.
 * - Updates the component state to COMPONENT_STATE_FAILED in case of errors.
 */
std::vector<EncodingParameters> PluginCommsTwoSixStubTransport::getActionParams(
    const Action &action) {
    TRACE_METHOD(action.actionId, action.json);

    try {
        ActionJson actionParams = nlohmann::json::parse(action.json);
        switch (actionParams.type) {
            case ACTION_FETCH:
                return {};
            case ACTION_POST:
                return {{actionParams.linkId, "*/*", true, {}}};
            default:
                logError(logPrefix +
                         "Unrecognized action type: " + nlohmann::json(actionParams.type).dump());
                break;
        }
    } catch (nlohmann::json::exception &err) {
        logError(logPrefix + "Error in action JSON: " + err.what());
    }

    sdk->updateState(COMPONENT_STATE_FAILED);
    return {};
}

/**
 * @brief Enqueues content for processing based on the specified action and encoding parameters.
 * 
 * Only "post" actions are supported for content queuing, a "fetch" takes no content to upload.
 *
 * This method handles the queuing of content for a specific link ID and action type. It parses
 * the action JSON to determine the type of action and performs the appropriate operation.
 *
 * @param params The encoding parameters containing the link ID and other metadata.
 * @param action The action to be performed, including its ID and JSON representation.
 * @param content The content to be enqueued, represented as a vector of bytes.
 * @return ComponentStatus Returns COMPONENT_OK if the operation is successful or no content
 *         needs to be queued. Returns COMPONENT_ERROR if an error occurs during processing.
 *
 * @note If the content is empty, the method skips processing and returns COMPONENT_OK.
 * @note If the action type is unrecognized or an error occurs while parsing the action JSON,
 *       the method logs an error and returns COMPONENT_ERROR.
 */
ComponentStatus PluginCommsTwoSixStubTransport::enqueueContent(
    const EncodingParameters &params, const Action &action, const std::vector<uint8_t> &content) {
    TRACE_METHOD(params.linkId, action.actionId, action.json, content.size());

    if (content.empty()) {
        logDebug(logPrefix + "Skipping enqueue content. Content size is 0.");
        return COMPONENT_OK;
    }

    try {
        ActionJson actionParams = nlohmann::json::parse(action.json);
        // If the action is for a wildcard link id, the component manager chooses the link and
        // specifies it when calling enqueueContent()
        actionToLinkIdMap[action.actionId] = params.linkId;
        switch (actionParams.type) {
            case ACTION_FETCH:
                // Nothing to be queued
                return COMPONENT_OK;

            case ACTION_POST:
                return links.get(params.linkId)->enqueueContent(action.actionId, content);

            default:
                logError(logPrefix +
                         "Unrecognized action type: " + nlohmann::json(actionParams.type).dump());
                break;
        }
    } catch (nlohmann::json::exception &err) {
        logError(logPrefix + "Error in action JSON: " + err.what());
    }

    return COMPONENT_ERROR;
}

/**
 * @brief Handles the dequeuing of content associated with a specific action. This is done when the user model updates its timeline and removes an action which has already had content enqueued, to enable triggering any messages in that content to be reencoded and requeued for a future action.
 *
 * This method processes the given action, extracts its parameters, and performs
 * the appropriate operation based on the action type. It primarily handles 
 * `ACTION_POST` type actions by delegating the dequeue operation to the 
 * corresponding link. For other action types, it assumes no content is associated.
 *
 * @param action The action object containing the action ID and JSON parameters.
 * @return ComponentStatus Returns COMPONENT_OK if the operation is successful 
 *         or no content is associated with the action type. Returns COMPONENT_ERROR 
 *         if an exception occurs during processing.
 *
 * @throws std::exception If an error occurs during JSON parsing or map access.
 */
ComponentStatus PluginCommsTwoSixStubTransport::dequeueContent(const Action &action) {
    TRACE_METHOD(action.actionId);

    try {
        ActionJson actionParams = nlohmann::json::parse(action.json);
        LinkID linkId = actionParams.linkId == "*" ? actionToLinkIdMap.at(action.actionId) :
                                                     actionParams.linkId;
        actionToLinkIdMap.erase(action.actionId);
        switch (actionParams.type) {
            case ACTION_POST:
                return links.get(linkId)->dequeueContent(action.actionId);

            default:
                // No content associated with any other action types
                return COMPONENT_OK;
        }
    } catch (std::exception &err) {
        logError(logPrefix + "Error: " + err.what());
    }

    return COMPONENT_ERROR;
}

/**
 * Executes the specified action.
 *
 * @param handles A vector of RaceHandles representing message send calls associated with content enqueued for the action. If the action succeeds, they are considered SENT, if it fails they are FAILED and requeued for sending by Raceboat.ACTION_FETCH
 * @param action The Action object containing details about the operation to perform.
 *               Includes an action ID, JSON parameters, and type.
 * @return ComponentStatus indicating the result of the operation:
 *         - COMPONENT_OK: Operation succeeded.
 *         - COMPONENT_ERROR: Operation encountered a non-fatal error.
 *         - COMPONENT_FATAL: Operation encountered a fatal error.
 *
 * The function supports two types of actions:
 * - ACTION_FETCH: Fetches data from one or more links. If the link ID is "*", it fetches
 *   data from all links. Otherwise, it fetches data from the specified link.
 * - ACTION_POST: Posts data to a specific link. If the link ID is "*", it attempts to
 *   retrieve the link ID from the actionToLinkIdMap. If no link exists for the wildcard
 *   action, the operation is skipped.
 *
 * If an unrecognized action type is provided, an error is logged and COMPONENT_ERROR is returned.
 * Exceptions during JSON parsing or other operations are caught, logged, and result in COMPONENT_ERROR.
 */
ComponentStatus PluginCommsTwoSixStubTransport::doAction(const std::vector<RaceHandle> &handles,
                                                         const Action &action) {
    TRACE_METHOD(handles, action.actionId);

    try {
        ActionJson actionParams = nlohmann::json::parse(action.json);
        LinkID linkId = actionParams.linkId;

        switch (actionParams.type) {
            case ACTION_FETCH:
                // this map shouldn't contain anything in the fetch case, but just in case, erase it
                actionToLinkIdMap.erase(action.actionId);

                // This exemplar treats wildcard fetches as a fetch on EVERY link
                // Real transports which do NOT "fetch" for all links in a single action
                // (e.g. checking multiple subforums) may need to round-robin fetch for
                // a single link at a time
                if (actionParams.linkId == "*") {
                    ComponentStatus status = COMPONENT_OK;
                    logInfo(logPrefix + "Fetching from all links");
                    auto linkMap = links.getMap();
                    logInfo(logPrefix + "links: " + std::to_string(linkMap.size()));
                    for (auto &link : linkMap) {
                        logInfo(logPrefix + "Fetching from link " + link.first);
                        ComponentStatus thisStatus = link.second->fetch();
                        if (thisStatus == COMPONENT_FATAL) {
                            return COMPONENT_FATAL;
                        } else if (thisStatus != COMPONENT_OK) {
                            // propagate error status, but continue because it's not fatal
                            status = thisStatus;
                        }
                    }
                    return status;
                } else {
                    logInfo(logPrefix + "Fetching from single link");
                    return links.get(linkId)->fetch();
                }

            case ACTION_POST:
                if (linkId == "*") {
                    auto it = actionToLinkIdMap.find(action.actionId);
                    if (it == actionToLinkIdMap.end()) {
                        logInfo(logPrefix +
                                "Skipping action because no link exists for wildcard action");
                        return COMPONENT_OK;
                    } else {
                        linkId = it->second;
                    }
                }
                actionToLinkIdMap.erase(action.actionId);
                return links.get(linkId)->post(std::move(handles), action.actionId);

            default:
                logError(logPrefix +
                         "Unrecognized action type: " + nlohmann::json(actionParams.type).dump());
                break;
        }
    } catch (std::exception &err) {
        logError(logPrefix + "Error: " + err.what());
    }

    return COMPONENT_ERROR;
}

#ifndef TESTBUILD
/**
 * @brief Creates a transport component based on the specified transport type.
 *
 * This function initializes and returns a new instance of the PluginCommsTwoSixStubTransport
 * using the provided SDK and configuration parameters.
 *
 * @param transport The name of the transport type to create.
 * @param sdk Pointer to the transport SDK instance used for initialization.
 * @param roleName The name of the role associated with the transport component.
 * @param pluginConfig Configuration details for the plugin, including the plugin directory.
 * @return A pointer to the newly created transport component.
 */
ITransportComponent *createTransport(const std::string &transport, ITransportSdk *sdk,
                                     const std::string &roleName,
                                     const PluginConfig &pluginConfig) {
    TRACE_FUNCTION(transport, roleName, pluginConfig.pluginDirectory);
    return new PluginCommsTwoSixStubTransport(sdk);
}
/**
 * @brief Destroys the given transport component by deallocating its memory.
 * 
 * This function is responsible for safely deleting the provided transport
 * component object. It ensures that the memory allocated for the object
 * is released, preventing memory leaks.
 * 
 * @param component Pointer to the transport component to be destroyed.
 *                  Must be a valid pointer or nullptr.
 */
void destroyTransport(ITransportComponent *component) {
    TRACE_FUNCTION();
    delete component;
}

const RaceVersionInfo raceVersion = RACE_VERSION;
#endif
