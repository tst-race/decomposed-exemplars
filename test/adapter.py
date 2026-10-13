#!/usr/bin/env python3
"""
adapter.py
Decomposed-exemplars' plugin-owned test adapter: encapsulates the
twoSixIndirect transport's whiteboard-backed link addressing (a hashtag posted
to/read from the twosix-whiteboard sidecar) so the central scenario
orchestrator (raceboat/test/integration/) never needs to know this plugin's
sidecar or address-format requirements.
"""

import json
import platform
import sys
import uuid
from pathlib import Path

try:
    # The orchestrator (raceboat/test/integration/generate_scenario.py) already puts its own
    # directory on sys.path before loading this module, so this is expected to succeed whenever
    # adapter.py is invoked as intended.
    from adapter_types import NodeContribution, NodeRequest
except ImportError:
    # Fallback for standalone use (e.g. running this file directly): only works if raceboat is
    # checked out as a sibling of this repository.
    _integration_dir = Path(__file__).resolve().parents[2] / "raceboat" / "test" / "integration"
    if not _integration_dir.is_dir():
        raise ImportError(
            "decomposed-exemplars adapter: could not import adapter_types. Either invoke this "
            "module via raceboat/test/integration's orchestrator, or check out raceboat as a "
            f"sibling of this repository (expected {_integration_dir})"
        ) from None
    sys.path.insert(0, str(_integration_dir))
    from adapter_types import NodeContribution, NodeRequest  # noqa: E402

# Default composition (see source/manifest.json "compositions"): transport is
# twoSixIndirect, usermodel is rapidUser, encoding is base64. This is also the
# channel gid used for --recv-channel/--send-channel.
DEFAULT_COMPOSITION = "twoSixIndirectComposition"
KIT_NAME = "PluginCommsTwoSixStubDecomposed"

# twoSixIndirect is backed by a shared whiteboard (see
# decomposed-exemplars/scripts/docker-compose.yml); required as a sidecar
# service any time this composition is used.
WHITEBOARD_HOSTNAME = "twosix-whiteboard"
WHITEBOARD_PORT = 5000
SIDECAR_SERVICES = {
    "twosix-redis": {
        "image": "redis:6.0.6",
        "healthcheck": {
            "test": ["CMD", "redis-cli", "ping"],
            "interval": "2s",
            "timeout": "2s",
            "retries": 15,
        },
    },
    "twosix-whiteboard": {
        "image": "ghcr.io/tst-race/race-core/twosix-whiteboard:main",
        "command": "-w 8",
        "hostname": WHITEBOARD_HOSTNAME,
        "depends_on": {"twosix-redis": {"condition": "service_healthy"}},
        "environment": {"REDIS_HOSTNAME": "twosix-redis"},
        "healthcheck": {
            "test": [
                "CMD",
                "python3",
                "-c",
                "import socket; socket.create_connection(('127.0.0.1', 5000), 2).close()",
            ],
            "interval": "2s",
            "timeout": "2s",
            "retries": 15,
        },
    },
}


def _detect_host_architecture() -> str:
    machine = platform.machine().lower()
    if machine in {"x86_64", "amd64"}:
        return "x86_64"
    if machine in {"arm64", "aarch64"}:
        return "arm64-v8a"
    raise ValueError(
        f"Unsupported host architecture for Docker builds: {platform.machine()}"
    )


def kit_dir(role: str) -> Path:
    # Both roles currently share the same built artifacts for this plugin
    # (role is accepted only to satisfy the adapter contract - see
    # adapter_types.py docstring).
    plugin_root = Path(__file__).resolve().parents[1]
    return (
        plugin_root
        / "kit"
        / "artifacts"
/ f"linux-{_detect_host_architecture()}-{'client' if role == 'connector' else 'server'}"
        / KIT_NAME
    )


def generate_node_contribution(request: NodeRequest) -> NodeContribution:
    # --param entries here must be prefixed with the composition id, not a
    # plugin id (see raceboat/source/plugin-loading/Config.cpp: "ensure
    # channelParameter.plugin matches plugin/composition id"), since
    # twoSixIndirectComposition is a manifest composition, not a plain plugin.
    composition = request.composition_name or DEFAULT_COMPOSITION
    contribution = NodeContribution(
        params={},
        channel_name=composition,
        kit_dir=kit_dir(request.role),
        sidecar_services=dict(SIDECAR_SERVICES),
    )

    # request.direction is only set (non-None) when this slot has been split
    # into two genuinely independent channels/compositions, one per
    # direction (see adapter_types.NodeRequest.direction).
    #
    # For bootstrap modes (slots "initial"/"final"): the listener's send and
    # connector's recv are dynamic (established per-connection via hello exchange)
    # UNLESS linkDirectionOverrides changes the effective link direction.
    # For client-connect/send/send-recv modes (slot "channel"): both directions
    # are explicit - listener creates both links, connector loads both.
    is_bootstrap_slot = request.slot in ("initial", "final")
    is_dynamic_direction = False
    if is_bootstrap_slot and request.direction:
        link_direction = request.effective_link_direction or "LD_BIDI"
        if request.role == "listener" and request.direction == "send":
            # Listener's send link (downstream): dynamic unless LD_CREATOR_TO_LOADER
            # LD_BIDI, LD_LOADER_TO_CREATOR -> dynamic (loaded from hello)
            # LD_CREATOR_TO_LOADER -> creator creates, not dynamic
            is_dynamic_direction = link_direction in ("LD_BIDI", "LD_LOADER_TO_CREATOR")
        elif request.role == "connector" and request.direction == "recv":
            # Connector's recv link (downstream): dynamic unless LD_CREATOR_TO_LOADER
            # LD_BIDI, LD_LOADER_TO_CREATOR -> dynamic (created in hello)
            # LD_CREATOR_TO_LOADER -> loader loads from creator, not dynamic
            is_dynamic_direction = link_direction in ("LD_BIDI", "LD_LOADER_TO_CREATOR")
    if is_dynamic_direction:
        return contribution

    if request.role == "listener":
        # For mixed slots (direction specified), only create/publish the
        # address for this specific direction. For simple modes (direction=None),
        # create both links and publish both addresses.
        
        # Determine if listener send creates or loads its address
        is_bootstrap_slot = request.slot in ("initial", "final")
        link_direction = request.effective_link_direction or "LD_BIDI"
        listener_send_creates = not (
            is_bootstrap_slot
            and request.direction == "send"
            and link_direction == "LD_LOADER_TO_CREATOR"
        )
        # listener_recv_creates is always True in bootstrap (listener creates upstream recv)
        listener_recv_creates = True

        if request.direction == "recv" and listener_recv_creates:
            recv_address = {
                "hashtag": f"twoSixIndirect_{uuid.uuid4().hex[:12]}",
                "hostname": WHITEBOARD_HOSTNAME,
                "port": WHITEBOARD_PORT,
            }
            contribution.address_output = {"recv": recv_address}
            contribution.cli_flags["recv-address"] = json.dumps(recv_address)
        elif request.direction == "send":
            if listener_send_creates:
                # Listener creates send address (LD_BIDI or LD_CREATOR_TO_LOADER)
                send_address = {
                    "hashtag": f"twoSixIndirect_{uuid.uuid4().hex[:12]}",
                    "hostname": WHITEBOARD_HOSTNAME,
                    "port": WHITEBOARD_PORT,
                }
                contribution.address_output = {"send": send_address}
                contribution.cli_flags["send-address"] = json.dumps(send_address)
            else:
                # Listener LOADS send address from connector (LD_LOADER_TO_CREATOR)
                contribution.needs_peer_address = True
                connector_addresses = next(iter(request.peer_context.values()), None)
                if connector_addresses is None:
                    raise ValueError(
                        "decomposed-exemplars adapter: listener node requires a "
                        "connector's address_output in peer_context for LD_LOADER_TO_CREATOR, "
                        "but none was provided"
                    )
                if isinstance(connector_addresses, dict) and "recv" in connector_addresses:
                    contribution.cli_flags["send-address"] = json.dumps(connector_addresses["recv"])
        else:
            # Simple mode: both directions
            # For bootstrap simple mode, we assume LD_BIDI (both create)
            recv_address = {
                "hashtag": f"twoSixIndirect_{uuid.uuid4().hex[:12]}",
                "hostname": WHITEBOARD_HOSTNAME,
                "port": WHITEBOARD_PORT,
            }
            send_address = {
                "hashtag": f"twoSixIndirect_{uuid.uuid4().hex[:12]}",
                "hostname": WHITEBOARD_HOSTNAME,
                "port": WHITEBOARD_PORT,
            }
            contribution.address_output = {"recv": recv_address, "send": send_address}
            contribution.cli_flags["recv-address"] = json.dumps(recv_address)
            contribution.cli_flags["send-address"] = json.dumps(send_address)
    elif request.role == "connector":
        # Determine if connector recv is dynamic or creates its own address
        is_bootstrap_slot = request.slot in ("initial", "final")
        link_direction = request.effective_link_direction or "LD_BIDI"
        connector_recv_creates = (
            is_bootstrap_slot
            and request.direction == "recv"
            and link_direction == "LD_LOADER_TO_CREATOR"
        )

        if connector_recv_creates:
            # Connector CREATES its own recv address (loader creates)
            # Listener will load this address for its send link
            recv_address = {
                "hashtag": f"twoSixIndirect_{uuid.uuid4().hex[:12]}",
                "hostname": WHITEBOARD_HOSTNAME,
                "port": WHITEBOARD_PORT,
            }
            contribution.address_output = {"recv": recv_address}
            contribution.cli_flags["recv-address"] = json.dumps(recv_address)
            # No send-address needed for this direction
        else:
            # Default: connector loads from listener's address
            contribution.needs_peer_address = True
            listener_addresses = next(iter(request.peer_context.values()), None)
            if listener_addresses is None:
                raise ValueError(
                    "decomposed-exemplars adapter: connector node requires a "
                    "listener's address_output in peer_context, but none was provided"
                )
            # Listener publishes recv and/or send addresses. Connector loads:
            # - send-address = listener's recv address (where connector sends, upstream)
            # - recv-address = listener's send address (where connector receives, downstream)
            if isinstance(listener_addresses, dict) and ("recv" in listener_addresses or "send" in listener_addresses):
                # Mixed slot format: listener published address(es) for specific direction(s)
                if request.direction == "send":
                    # Connector sends upstream -> needs listener's recv address
                    if "recv" in listener_addresses:
                        contribution.cli_flags["send-address"] = json.dumps(listener_addresses["recv"])
                elif request.direction == "recv":
                    # Connector receives downstream -> needs listener's send address
                    if "send" in listener_addresses:
                        contribution.cli_flags["recv-address"] = json.dumps(listener_addresses["send"])
                else:
                    # Simple mode: listener published both addresses
                    if "recv" in listener_addresses:
                        contribution.cli_flags["send-address"] = json.dumps(listener_addresses["recv"])
                    if "send" in listener_addresses:
                        contribution.cli_flags["recv-address"] = json.dumps(listener_addresses["send"])
            else:
                # Legacy format: only recv address provided (for bootstrap modes)
                contribution.cli_flags["send-address"] = json.dumps(listener_addresses)
    else:
        raise ValueError(f"decomposed-exemplars adapter: unknown role '{request.role}'")

    return contribution

