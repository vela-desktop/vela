#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
"""Archive GitHub-side aggregate metrics; never Vela runtime telemetry."""

import argparse
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import time
import urllib.error
import urllib.request

SCHEMA_VERSION = 1
RETRY_HTTP = {408, 500, 502, 503, 504}


def natural(value, field):
    if type(value) is not int or value < 0:
        raise ValueError(f"Invalid nonnegative integer: {field}")
    return value


def github_json(url, token):
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "vela-distribution-metrics",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = "Bearer " + token
    for attempt in range(3):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            h = error.headers or {}
            limited = error.code in (403, 429) and (
                error.code == 429 or h.get("Retry-After") or h.get("X-RateLimit-Remaining") == "0"
            )
            if not (limited or error.code in RETRY_HTTP) or attempt == 2:
                raise RuntimeError(f"GitHub API HTTP {error.code} at {url}") from None
            delay = 2 ** attempt
            if limited:
                try:
                    if h.get("Retry-After"):
                        delay = max(delay, float(h["Retry-After"]))
                    elif h.get("X-RateLimit-Remaining") == "0":
                        delay = max(delay, int(h["X-RateLimit-Reset"]) - time.time() + 1)
                    else:
                        delay = 60
                except (ValueError, TypeError, KeyError):
                    delay = 60
                if delay > 120:
                    raise RuntimeError(f"GitHub API rate limit exceeds retry budget at {url}") from None
            time.sleep(max(0, delay))
        except (urllib.error.URLError, TimeoutError) as error:
            if attempt == 2:
                raise RuntimeError(f"GitHub API network failure at {url}: {type(error).__name__}") from None
            time.sleep(2 ** attempt)


def paginate(url, token):
    page = 1
    while True:
        separator = "&" if "?" in url else "?"
        items = github_json(f"{url}{separator}per_page=100&page={page}", token)
        if not isinstance(items, list):
            raise ValueError(f"Unexpected API list response at {url}")
        yield from items
        if len(items) < 100:
            break
        page += 1


def read_json(path, default):
    if not path.exists():
        return default
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as stream:
        json.dump(data, stream, indent=2, sort_keys=True, ensure_ascii=False)
        stream.write("\n")


def merge_traffic(history, kind, response):
    entries = response.get(kind)
    if not isinstance(entries, list):
        raise ValueError(f"Missing daily {kind} array")
    days = history.setdefault(kind, {})
    for entry in entries:
        date = datetime.strptime(entry["timestamp"][:10], "%Y-%m-%d").date().isoformat()
        days[date] = {
            "count": natural(entry["count"], f"{kind}.count"),
            "uniques": natural(entry["uniques"], f"{kind}.uniques"),
        }


def popular(entries, kind):
    if not isinstance(entries, list):
        raise ValueError(f"Expected popular {kind} array")
    key = "referrer" if kind == "referrers" else "path"
    normalized = []
    for entry in entries:
        if not isinstance(entry[key], str):
            raise ValueError(f"Invalid popular {key}")
        item = {
            key: entry[key], "count": natural(entry["count"], "count"),
            "uniques": natural(entry["uniques"], "uniques"),
        }
        if key == "path":
            if not isinstance(entry.get("title", ""), str):
                raise ValueError("Invalid path title")
            item["title"] = entry.get("title", "")
        normalized.append(item)
    return normalized


def downloads_delta(previous, current):
    return sum(max(0, asset["downloads"] - previous.get(key, {}).get("downloads", 0))
               for key, asset in current.items())


def track_asset_history(history, assets, timestamp):
    history["schema_version"] = SCHEMA_VERSION
    recorded = history.setdefault("assets", {})
    for asset_id, asset in assets.items():
        old = recorded.get(asset_id, {})
        recorded[asset_id] = {
            "name": asset["name"], "release_tag": asset["release_tag"],
            "first_seen_at": old.get("first_seen_at", timestamp),
            "last_seen_at": timestamp,
            "max_downloads_observed": max(old.get("max_downloads_observed", 0), asset["downloads"]),
        }
    return history


def window(series, end, length):
    start = end - timedelta(days=length - 1)
    days = [(start + timedelta(days=i)).isoformat() for i in range(length)]
    samples = [series[day]["count"] for day in days if day in series]
    return {
        "start_date": start.isoformat(), "end_date": end.isoformat(),
        "count": sum(samples), "observed_days": len(samples),
        "complete": len(samples) == length,
    }


def summarize_traffic(history, today):
    end = today - timedelta(days=1)
    result = {}
    for kind in ("clones", "views"):
        series = history.get(kind, {})
        recent = window(series, end, 7)
        prior = window(series, end - timedelta(days=7), 7)
        month = window(series, end, 30)
        growth = None
        if recent["complete"] and prior["complete"] and prior["count"] > 0:
            growth = round((recent["count"] - prior["count"]) * 100 / prior["count"], 2)
        result[kind] = {
            "last_7_complete_days": recent,
            "previous_7_complete_days": prior,
            "last_30_complete_days": month,
            "growth_vs_previous_7_days_percent": growth,
        }
    return result


def summarize_releases(assets):
    groups = {}
    for asset_id, asset in assets.items():
        groups.setdefault(asset["release_tag"], []).append({
            "id": asset_id, "name": asset["name"], "downloads": asset["downloads"],
        })
    return [
        {"tag": tag,
         "downloads_current_assets": sum(item["downloads"] for item in values),
         "assets": sorted(values, key=lambda item: (item["name"], item["id"]))}
        for tag, values in sorted(groups.items())
    ]


def collect(repo, output, releases_token, traffic_token, now=None):
    now = now or datetime.now(timezone.utc)
    if now.tzinfo is None or now.utcoffset() != timedelta(0):
        raise ValueError("Collection time must be UTC")
    today = now.date()
    day = today.isoformat()
    timestamp = now.isoformat(timespec="seconds")
    api = f"https://api.github.com/repos/{repo}"
    data = output / "data"

    releases = list(paginate(api + "/releases", releases_token))
    assets = {}
    for release in releases:
        for item in paginate(api + f"/releases/{release['id']}/assets", releases_token):
            assets[str(natural(item["id"], "asset.id"))] = {
                "name": item["name"],
                "release_tag": release["tag_name"],
                "downloads": natural(item["download_count"], "asset.download_count"),
            }

    traffic_path = data / "traffic-daily.json"
    history = read_json(traffic_path, {"schema_version": SCHEMA_VERSION, "clones": {}, "views": {}})
    referrers = paths = None
    traffic_status = "not_configured"
    if traffic_token:
        for kind in ("clones", "views"):
            merge_traffic(history, kind, github_json(api + f"/traffic/{kind}?per=day", traffic_token))
        referrers = popular(github_json(api + "/traffic/popular/referrers", traffic_token), "referrers")
        paths = popular(github_json(api + "/traffic/popular/paths", traffic_token), "paths")
        traffic_status = "ok"
        history["schema_version"] = SCHEMA_VERSION
        history["last_collected_at"] = timestamp

    snapshots = data / "release-snapshots"
    earlier = sorted(path for path in snapshots.glob("*.json") if path.stem < day)
    previous_date = earlier[-1].stem if earlier else None
    previous = read_json(earlier[-1], {}) if earlier else None
    delta = downloads_delta(previous["assets"], assets) if previous else None
    asset_history = track_asset_history(
        read_json(data / "release-asset-history.json", {"schema_version": SCHEMA_VERSION, "assets": {}}),
        assets, timestamp,
    )
    total = sum(asset["downloads"] for asset in assets.values())
    observed = sum(asset["max_downloads_observed"] for asset in asset_history["assets"].values())
    latest = {
        "schema_version": SCHEMA_VERSION, "collected_at": timestamp, "repository": repo,
        "traffic_status": traffic_status, "releases": len(releases),
        "release_assets": len(assets), "release_asset_downloads_total": total,
        "release_asset_downloads_observed_maxima": observed,
        "downloads_since_previous_snapshot": delta,
        "previous_snapshot_date": previous_date,
    }
    summary = {
        "schema_version": SCHEMA_VERSION, "generated_at": timestamp, "repository": repo,
        "sources": {
            "traffic": {"status": traffic_status, "last_collected_at": history.get("last_collected_at")},
            "releases": {"status": "ok", "last_collected_at": timestamp},
        },
        "traffic": summarize_traffic(history, today) if traffic_status == "ok" else None,
        "releases": {
            "release_count": len(releases), "asset_count": len(assets),
            "downloads_current_assets": total, "downloads_observed_maxima": observed,
            "downloads_since_previous_daily_snapshot": delta,
            "previous_snapshot_date": previous_date, "by_release": summarize_releases(assets),
        },
        "top_referrers": referrers,
        "popular_paths": paths,
    }

    # Validate every upstream response before changing any output file.
    if traffic_status == "ok":
        write_json(traffic_path, history)
        common = {
            "schema_version": SCHEMA_VERSION, "collected_at": timestamp,
            "window": "rolling_14_days", "coverage": "top_10_only",
        }
        write_json(data / "referrers.json", {**common, "items": referrers})
        write_json(data / "popular-paths.json", {**common, "items": paths})
        write_json(data / "discovery-snapshots" / f"{day}.json", {
            **common, "referrers": referrers, "popular_paths": paths,
        })
    write_json(data / "release-asset-history.json", asset_history)
    write_json(snapshots / f"{day}.json", {
        "schema_version": SCHEMA_VERSION, "collected_at": timestamp,
        "releases": len(releases), "assets": assets, "total_downloads_current_assets": total,
    })
    write_json(data / "latest.json", latest)
    write_json(data / "summary.json", summary)
    (output / "README.md").write_text(
        "# Vela distribution metrics archive\n\n"
        "Public aggregate GitHub statistics; no Vela runtime telemetry.\n\n"
        "- data/summary.json: dashboard-friendly trends and release breakdown.\n"
        "- data/latest.json: latest collection status and counts.\n"
        "- data/traffic-daily.json: persistent per-day clone/view history.\n"
        "- data/referrers.json and data/popular-paths.json: latest rolling top-ten lists.\n"
        "- data/discovery-snapshots/: archived overlapping rolling-window rankings.\n"
        "- data/release-snapshots/: daily Release asset counters.\n"
        "- data/release-asset-history.json: observed maxima, including removed assets.\n\n"
        "Gaps are unknown, not zero. Daily unique counts cannot be added together.\n"
        "Observed maxima are not exact all-time downloads.\n",
        encoding="utf-8",
    )
    print(json.dumps(latest, indent=2, sort_keys=True))
    return latest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repo", default=os.getenv("GITHUB_REPOSITORY", "vela-desktop/vela"))
    args = parser.parse_args()
    collect(args.repo, args.output, os.getenv("GITHUB_TOKEN", ""), os.getenv("VELA_TRAFFIC_TOKEN", ""))


if __name__ == "__main__":
    main()
