#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Rent a single-GPU NVIDIA container on Vast.ai as a one-job GitHub runner.

`start` ranks eligible offers by price, CPU capacity, reliability and driver
within a price cap, starts the CI image with a just-in-time runner and waits until
the runner is online; an offer that fails is destroyed and the next one
tried. `stop` destroys every instance and runner of a label, so it also
cleans up after a `start` that was cancelled halfway.

Needs VAST_API_KEY (instances: read and write) and GH_TOKEN (organization
self-hosted runners: read and write).
"""

import argparse
import itertools
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
# A host with driver 580.173 gave Vulkan no dmabuf extensions, one with
# 595.84 did: newer drivers are tried first, still under the price cap.
PREFERRED_DRIVER = 590
ATTEMPTS = 5
# A slow/stuck image pull must not consume the old 12-minute allowance.
# Once Vast reports running, the JIT runner should register much sooner.
IMAGE_LOAD_TIMEOUT = 6 * 60
RUNNER_READY_TIMEOUT = 150
POLL_INTERVAL = 15
# Status strings seen during failed Vast image pulls; trying a different host
# is preferable to waiting for an already failed pull to time out.
FATAL_STARTUP_MESSAGES = (
    "could not resolve host",
    "temporary failure in name resolution",
    "no such host",
    "manifest unknown",
    "unauthorized",
    "no space left on device",
    "failed to pull",
)


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


def offer_rank(offer):
    """Prefer value from reliable hosts, not only the lowest hourly price.

    GPU tests run serially, so CPU gains have diminishing returns. Weight
    effective CPU count sublinearly and cap it at 24 to avoid overpaying for
    large shared hosts; reliability penalizes hosts close to the 95% floor.
    A suitable recent driver is still the first priority.
    """
    cpus = min(float(offer["cpu_cores_effective"]), 24.0)
    reliability = float(offer["reliability2"])
    price = float(offer["dph_total"])
    value = price / (cpus / 8.0) ** 0.5 * (1.0 + 3.0 * (1.0 - reliability))
    return (driver_major(offer) < PREFERRED_DRIVER, value, -reliability, price)


def choose(offers, max_price, gpu=None, tried=()):
    fitting = (o for o in offers if suitable(o, max_price, gpu) and o.get("machine_id") not in tried)
    return sorted(fitting, key=offer_rank)


def search(max_price, gpu=None):
    query = {
        "gpu_name": {"in": [gpu] if gpu else list(GPUS)},
        "num_gpus": {"eq": 1},
        "rentable": {"eq": True},
        "rented": {"eq": False},
        "verified": {"eq": True},
        "dph_total": {"lte": max_price},
        # Server side too, so that unfit offers don't fill the limit; suitable()
        # still checks everything.
        "reliability2": {"gte": 0.95},
        "cpu_cores_effective": {"gte": 8},
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
    found = []
    for page in itertools.count(1):
        batch = github("GET", f"/orgs/{org}/actions/runners?per_page=100&page={page}").get("runners", [])
        found += batch
        if len(batch) < 100:
            break
    return [r for r in found if r.get("name", "").startswith(label)]


def stop(org, label):
    for instance in instances(label):
        vast("DELETE", f"/v0/instances/{instance['id']}/")
        print(f"Destroyed Vast instance {instance['id']}")
    for runner in runners(org, label):
        github("DELETE", f"/orgs/{org}/actions/runners/{runner['id']}")
        print(f"Removed runner {runner['name']}")


def wait_online(org, runner_id, instance_id):
    start = time.monotonic()
    running_since = None
    last = None
    while True:
        # Read the runner first: it can register before Vast reports running.
        if github("GET", f"/orgs/{org}/actions/runners/{runner_id}").get("status") == "online":
            return True
        state = next((i for i in vast("GET", "/v1/instances/").get("instances", [])
                      if i.get("id") == instance_id), None)
        if state is None or state.get("actual_status") in ("exited", "offline", "error", "failed"):
            print(f"Instance {instance_id} stopped: {state and state.get('status_msg')}", flush=True)
            return False
        now = (state.get("actual_status"), (state.get("status_msg") or "").strip()[:200])
        if now != last:
            print(f"  {int(time.monotonic() - start)} s: {now[0]}, {now[1]}", flush=True)
            last = now
        status, message = now
        if status == "running":
            if running_since is None:
                running_since = time.monotonic()
            if time.monotonic() - running_since >= RUNNER_READY_TIMEOUT:
                print(f"Runner not online {RUNNER_READY_TIMEOUT} s after instance "
                      f"{instance_id} started; trying another host", flush=True)
                return False
        else:
            if any(error in message.lower() for error in FATAL_STARTUP_MESSAGES):
                print(f"Instance {instance_id} image startup failed; trying another host", flush=True)
                return False
            if time.monotonic() - start >= IMAGE_LOAD_TIMEOUT:
                print(f"Image not running after {IMAGE_LOAD_TIMEOUT} s on instance "
                      f"{instance_id}; trying another host", flush=True)
                return False
        time.sleep(POLL_INTERVAL)


def start(org, label, image, runner_version, max_price, gpu):
    tried = set()
    for attempt in range(1, ATTEMPTS + 1):
        # Offers come and go: search again for every attempt.
        candidates = choose(search(max_price, gpu), max_price, gpu, tried)
        if not candidates:
            sys.exit(f"No single {gpu or 'RTX'} offer under ${max_price}/h fits"
                     + (f" after {attempt - 1} attempts" if tried else ""))
        offer = candidates[0]
        tried.add(offer.get("machine_id"))
        name = f"{label}-{attempt}"
        summary = (f"{offer['gpu_name']}, ${offer['dph_total']:.3f}/h, {offer.get('geolocation')}, "
                   f"driver {offer.get('driver_version')}, {offer['cpu_cores_effective']:g} CPUs")
        print(f"Attempt {attempt}: offer {offer['id']} ({summary})", flush=True)
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
        try:
            online = bool(instance_id) and wait_online(org, jit["runner"]["id"], instance_id)
        except (RuntimeError, urllib.error.URLError, TimeoutError) as error:
            print(f"Runner {name} startup probe failed: {error}", flush=True)
            online = False
        if online:
            print(f"Runner {name} online on instance {instance_id}")
            # The job's outputs: only the file the runner names, never a path
            # from the command line.
            if output := os.environ.get("GITHUB_OUTPUT"):
                with open(output, "a", encoding="utf-8") as out:
                    out.write(f"gpu={summary}\n")
                    out.write(f"cpu_cores={offer['cpu_cores_effective']}\n")
            return
        stop(org, name)
    sys.exit(f"No runner came online after {ATTEMPTS} offers")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=("start", "stop"))
    parser.add_argument("--org", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--image")
    parser.add_argument("--runner-version")
    parser.add_argument("--max-price", type=float, default=0.12)
    parser.add_argument("--gpu", default="", help="One GPU from the list instead of the cheapest")
    args = parser.parse_args()
    if args.gpu and args.gpu not in GPUS:
        parser.error(f"--gpu must be one of: {', '.join(GPUS)}")
    if args.command == "stop":
        stop(args.org, args.label)
    elif not args.image or not args.runner_version:
        parser.error("start needs --image and --runner-version")
    else:
        start(args.org, args.label, args.image, args.runner_version, args.max_price, args.gpu or None)


if __name__ == "__main__":
    main()
