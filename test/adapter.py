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


def _kit_dir() -> Path:
    plugin_root = Path(__file__).resolve().parents[1]
    return (
        plugin_root
        / "kit"
        / "artifacts"
        / f"linux-{_detect_host_architecture()}-server"
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
        kit_dir=_kit_dir(),
        sidecar_services=dict(SIDECAR_SERVICES),
    )

    # request.direction is only set (non-None) when this slot has been split
    # into two genuinely independent channels/compositions, one per
    # direction (see adapter_types.NodeRequest.direction). In that case, only
    # the listener's "recv" link and the connector's "send" link get an
    # explicit, out-of-band-published address; the other direction on each
    # side is established dynamically per-connection (the listener loads a
    # reply link address embedded in the dialer's hello message, and the
    # dialer self-creates its recv link and advertises it the same way - see
    # raceboat/source/state-machine/DialStateMachine.cpp's StateDialSendOpen
    # and ListenStateMachine.cpp's StateListenWaitingForSendConnection).
    is_dynamic_direction = request.direction == "send" if request.role == "listener" else (
        request.direction == "recv" if request.role == "connector" else False
    )
    if is_dynamic_direction:
        return contribution

    if request.role == "listener":
        # createLink()'s default hashtag is derived from the running race
        # persona (see PluginCommsTwoSixStubTransport.cpp), which we can't
        # predict from outside the container. Set --recv-address explicitly
        # instead so the same value can be published for connectors to load.
        address = {
            "hashtag": f"twoSixIndirect_{uuid.uuid4().hex[:12]}",
            "hostname": WHITEBOARD_HOSTNAME,
            "port": WHITEBOARD_PORT,
        }
        contribution.address_output = address
        contribution.cli_flags["recv-address"] = json.dumps(address)
    elif request.role == "connector":
        contribution.needs_peer_address = True
        listener_address = next(iter(request.peer_context.values()), None)
        if listener_address is None:
            raise ValueError(
                "decomposed-exemplars adapter: connector node requires a "
                "listener's address_output in peer_context, but none was provided"
            )
        contribution.cli_flags["send-address"] = json.dumps(listener_address)
    else:
        raise ValueError(f"decomposed-exemplars adapter: unknown role '{request.role}'")

    return contribution

