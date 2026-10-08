
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

#ifndef __COMMS_TWOSIX_TRANSPORT_H__
#define __COMMS_TWOSIX_TRANSPORT_H__

#include <ChannelProperties.h>
#include <ITransportComponent.h>
#include <LinkProperties.h>

#include <atomic>

#include <algorithm>
#include <mutex>
#include <unordered_set>

#include "LinkMap.h"

class PluginCommsTwoSixStubTransport : public ITransportComponent {
public:
    explicit PluginCommsTwoSixStubTransport(ITransportSdk *sdk);

    virtual ComponentStatus onUserInputReceived(RaceHandle handle, bool answered,
                                                const std::string &response) override;

    virtual TransportProperties getTransportProperties() override;

    virtual LinkProperties getLinkProperties(const LinkID &linkId) override;

    virtual ComponentStatus createLink(RaceHandle handle, const LinkID &linkId) override;

    virtual ComponentStatus loadLinkAddress(RaceHandle handle, const LinkID &linkId,
                                            const std::string &linkAddress) override;

    virtual ComponentStatus loadLinkAddresses(RaceHandle handle, const LinkID &linkId,
                                              const std::vector<std::string> &linkAddress) override;

    virtual ComponentStatus createLinkFromAddress(RaceHandle handle, const LinkID &linkId,
                                                  const std::string &linkAddress) override;

    virtual ComponentStatus destroyLink(RaceHandle handle, const LinkID &linkId) override;

    virtual std::vector<EncodingParameters> getActionParams(const Action &action) override;

    virtual ComponentStatus enqueueContent(const EncodingParameters &params, const Action &action,
                                           const std::vector<uint8_t> &content) override;

    virtual ComponentStatus dequeueContent(const Action &action) override;

    virtual ComponentStatus doAction(const std::vector<RaceHandle> &handles,
                                     const Action &action) override;

protected:
    virtual std::shared_ptr<Link> createLinkInstance(const LinkID &linkId,
                                                     const LinkAddress &address,
                                                     const LinkProperties &properties);

private:
    ITransportSdk *sdk;
    std::string racePersona;
    ChannelProperties channelProperties;
    LinkProperties defaultLinkProperties;

    LinkMap links;

    // Guards deletedLinks and actionToLinkIdMap, and serializes them against links add/remove so a
    // link can never be observed as both present in links and absent from deletedLinks (or vice
    // versa), and so actionToLinkIdMap lookups/erases cannot race with each other.
    mutable std::mutex linkStateMutex;
    std::unordered_set<LinkID> deletedLinks;

    std::unordered_map<uint64_t, LinkID> actionToLinkIdMap;

    // actionIds whose actionToLinkIdMap entry was dropped by purgeStaleActionsForLinkLocked()
    // before doAction() could resolve them, so doAction() still knows to report a terminal
    // package failure instead of silently skipping handles that were never otherwise failed.
    std::unordered_set<uint64_t> staleActionIds;

    // Next available hashtag suffix.
    // TODO: should probably pull from a pool of tags (randomly generated?) instead so we can reuse
    // old tags. Although I'm guessing with a 64 bit int it's unlikely this will ever rollover
    // (famous last words?) - GP
    std::atomic<int64_t> nextAvailableHashTag{0};

    bool preLinkCreate(const std::string &logPrefix, RaceHandle handle, const LinkID &linkId,
                       LinkSide invalidRoleLinkSide);
    ComponentStatus postLinkCreate(const std::string &logPrefix, RaceHandle handle,
                                   const LinkID &linkId, const LinkAddress &address,
                                   const LinkProperties &properties, LinkStatus linkStatus);

    // Returns true if linkId is tombstoned and the caller should skip the action. Must be called
    // while holding linkStateMutex.
    bool isDeletedLocked(const LinkID &linkId) const;

    // Drops the tombstone for linkId once no pending action still maps to it, to bound the set's
    // size instead of retaining every destroyed link ID indefinitely. Must be called while holding
    // linkStateMutex.
    void pruneDeletedLinkIfUnreferencedLocked(const LinkID &linkId);

    // Erases any actionToLinkIdMap entries for linkId, so stale actions queued against a destroyed
    // link instance can't be misapplied once the ID is reused by a newly created link. Must be
    // called while holding linkStateMutex.
    void purgeStaleActionsForLinkLocked(const std::string &logPrefix, const LinkID &linkId);
};

#endif  // __COMMS_TWOSIX_TRANSPORT_H__
