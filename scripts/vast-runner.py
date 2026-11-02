#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rent a single-GPU NVIDIA container on Vast.ai as a one-job GitHub runner.

`start` picks the cheapest offer that can build and test Vela under a price
cap, starts the CI image there with a just-in-time runner and waits until
the runner is online; an offer that fails is destroyed and the next one
tried. `stop` destroys every instance and runner of a label, so it also
cleans up after a `start` that was cancelled halfway.

Needs VAST_API_KEY (instances: read and write) and GH_TOKEN (organization
self-hosted runners: read and write).
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

VAST = "https://console.vast.ai/api"
GITHUB = "https://api.github.com"
# GeForce RTX cards, from the RTX 2060 up: the drivers Vulkan 1.4 needs no
# longer support anything before Turing. The price cap decides among them.
GPUS = (
    "RTX 2060", "RTX 2060S", "RTX 2070", "RTX 2070S", "RTX 2080", "RTX 2080S", "RTX 2080 Ti",
    "RTX 3050", "RTX 3060", "RTX 3060 Ti", "RTX 3070", "RTX 3070 Ti",
    "RTX 3080", "RTX 3080 Ti", "RTX 3090", "RTX 3090 Ti",
    "RTX 4060", "RTX 4060 Ti", "RTX 4070", "RTX 4070S", "RTX 4070 Ti", "RTX 4070S Ti",
    "RTX 4080", "RTX 4080S", "RTX 4090",
    "RTX 5060", "RTX 5060 Ti", "RTX 5070", "RTX 5070 Ti", "RTX 5080", "RTX 5090",
)
DISK_GB = 40
MIN_DRIVER = 570  # Vulkan 1.4
ATTEMPTS = 4
ONLINE_TIMEOUT = 15 * 60


def request(method, url, headers, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method, headers={
        "Content-Type": "application/json", "User-Agent": "vela-vast-runner", **headers})
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            text = response.read()
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")[:300]
        raise RuntimeError(f"{method} {url.split('?')[0]}: HTTP {error.code} {detail}") from None
    return json.loads(text) if text.strip() else {}


def vast(method, path, body=None):
    return request(method, VAST + path, {"Authorization": "Bearer " + os.environ["VAST_API_KEY"]}, body)


def github(method, path, body=None):
    return request(method, GITHUB + path, {
        "Authorization": "Bearer " + os.environ["GH_TOKEN"],
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
    }, body)


def driver_major(offer):
    try:
        return int(str(offer.get("driver_version", "")).split(".")[0])
    except ValueError:
        return 0


def suitable(offer, max_price, gpu=None):
    """Whether an offer can build and test Vela, alone, under the price cap."""
    return (offer.get("num_gpus") == 1
            and offer.get("gpu_name") in ((gpu,) if gpu else GPUS)
            and offer.get("dph_total", float("inf")) <= max_price
            and offer.get("verification") == "verified"
            and offer.get("reliability2", 0) >= 0.95
            and offer.get("disk_space", 0) >= DISK_GB
            and offer.get("cpu_cores_effective", 0) >= 8
            and offer.get("cpu_ram", 0) >= 16000
            and offer.get("gpu_ram", 0) >= 6000
            and offer.get("inet_down", 0) >= 200
            and driver_major(offer) >= MIN_DRIVER)


def choose(offers, max_price, gpu=None):
    return sorted((o for o in offers if suitable(o, max_price, gpu)), key=lambda o: o["dph_total"])


def search(max_price, gpu=None):
    query = {
        "gpu_name": {"in": [gpu] if gpu else list(GPUS)},
        "num_gpus": {"eq": 1},
        "rentable": {"eq": True},
        "rented": {"eq": False},
        "verified": {"eq": True},
        "dph_total": {"lte": max_price},
        "allocated_storage": DISK_GB,
        "type": "on-demand",
        "order": [["dph_total", "asc"]],
        "limit": 200,
    }
    return vast("GET", "/v0/bundles/?q=" + urllib.parse.quote(json.dumps(query))).get("offers", [])


def instances(label):
    return [i for i in vast("GET", "/v1/instances/").get("instances", [])
            if (i.get("label") or "").startswith(label)]


def runners(org, label):
    found = github("GET", f"/orgs/{org}/actions/runners?per_page=100").get("runners", [])
    return [r for r in found if r.get("name", "").startswith(label)]


def stop(org, label):
    for instance in instances(label):
        vast("DELETE", f"/v0/instances/{instance['id']}/")
        print(f"Destroyed Vast instance {instance['id']}")
    for runner in runners(org, label):
        github("DELETE", f"/orgs/{org}/actions/runners/{runner['id']}")
        print(f"Removed runner {runner['name']}")


def wait_online(org, runner_id, instance_id):
    deadline = time.monotonic() + ONLINE_TIMEOUT
    while time.monotonic() < deadline:
        if github("GET", f"/orgs/{org}/actions/runners/{runner_id}").get("status") == "online":
            return True
        state = next((i for i in vast("GET", "/v1/instances/").get("instances", [])
                      if i.get("id") == instance_id), None)
        if state is None or state.get("actual_status") in ("exited", "offline"):
            print(f"Instance {instance_id} stopped: {state and state.get('status_msg')}")
            return False
        time.sleep(15)
    print(f"Runner not online after {ONLINE_TIMEOUT // 60} minutes")
    return False


def start(org, label, image, runner_version, max_price, gpu, output):
    candidates = choose(search(max_price, gpu), max_price, gpu)
    if not candidates:
        sys.exit(f"No single {gpu or 'Turing or later GeForce'} offer under ${max_price}/h fits")
    for attempt, offer in enumerate(candidates[:ATTEMPTS], 1):
        name = f"{label}-{attempt}"
        summary = (f"{offer['gpu_name']}, ${offer['dph_total']:.3f}/h, {offer.get('geolocation')}, "
                   f"driver {offer.get('driver_version')}, {offer['cpu_cores_effective']:g} CPUs")
        print(f"Attempt {attempt}: offer {offer['id']} ({summary})")
        jit = github("POST", f"/orgs/{org}/actions/runners/generate-jitconfig", {
            "name": name, "runner_group_id": 1, "labels": [label], "work_folder": "_work"})
        try:
            created = vast("PUT", f"/v0/asks/{offer['id']}/", {
                "client_id": "me",
                "image": image,
                "disk": DISK_GB,
                "label": name,
                "runtype": "args",
                "args": ["/usr/local/bin/vela-ci-runner"],
                "env": {"VELA_RUNNER_JITCONFIG": jit["encoded_jit_config"],
                        "VELA_RUNNER_VERSION": runner_version},
            })
        except RuntimeError as error:
            print(f"Offer {offer['id']} not rented: {error}")
            github("DELETE", f"/orgs/{org}/actions/runners/{jit['runner']['id']}")
            continue
        instance_id = created.get("new_contract")
        if instance_id and wait_online(org, jit["runner"]["id"], instance_id):
            print(f"Runner {name} online on instance {instance_id}")
            if output:
                with open(output, "a", encoding="utf-8") as out:
                    out.write(f"gpu={summary}\n")
            return
        stop(org, name)
    sys.exit(f"No runner came online after {min(len(candidates), ATTEMPTS)} offers")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("start", "stop"))
    parser.add_argument("--org", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--image")
    parser.add_argument("--runner-version")
    parser.add_argument("--max-price", type=float, default=0.12)
    parser.add_argument("--gpu", default="", help="One GPU from the list instead of the cheapest")
    parser.add_argument("--output", default=os.environ.get("GITHUB_OUTPUT"))
    args = parser.parse_args()
    if args.gpu and args.gpu not in GPUS:
        parser.error(f"--gpu must be one of: {', '.join(GPUS)}")
    if args.command == "stop":
        stop(args.org, args.label)
    elif not args.image or not args.runner_version:
        parser.error("start needs --image and --runner-version")
    else:
        start(args.org, args.label, args.image, args.runner_version, args.max_price, args.gpu or None,
              args.output)


if __name__ == "__main__":
    main()
