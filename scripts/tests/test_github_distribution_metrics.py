# SPDX-FileCopyrightText: 2026 Brando Giuffrida
# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from datetime import datetime, timedelta, timezone
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
from io import BytesIO

spec = importlib.util.spec_from_file_location("collector", Path(__file__).parents[1] / "github_distribution_metrics.py")
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
NOW = datetime(2026, 10, 10, 3, 17, tzinfo=timezone.utc)


def api(url, token):
    if url.endswith("/releases?per_page=100&page=1"):
        return [{"id": 20, "tag_name": "v1"}]
    if "/releases/20/assets?" in url:
        return [{"id": 7, "name": "vela.rpm", "download_count": 10}]
    for kind in ("clones", "views"):
        if f"/traffic/{kind}" in url:
            return {kind: [{"timestamp": (NOW.date() - timedelta(days=i)).isoformat() + "T00:00:00Z",
                            "count": 3, "uniques": 2} for i in range(14)]}
    if url.endswith("/traffic/popular/referrers"):
        return [{"referrer": "Google", "count": 12, "uniques": 9}]
    if url.endswith("/traffic/popular/paths"):
        return [{"path": "/vela-desktop/vela", "title": "Vela", "count": 8, "uniques": 5}]
    raise AssertionError(url)


class MetricsTests(unittest.TestCase):
    def test_download_deltas_and_deleted_assets(self):
        old = {"1": {"downloads": 12}, "2": {"downloads": 7}}
        self.assertEqual(m.downloads_delta(old, {"1": {"downloads": 15}, "3": {"downloads": 4}}), 7)
        self.assertEqual(m.downloads_delta(old, {"1": {"downloads": 5}}), 0)
        history = {"assets": {"1": {"name": "old", "release_tag": "v0",
                   "first_seen_at": "before", "last_seen_at": "before", "max_downloads_observed": 12}}}
        m.track_asset_history(history, {"2": {"name": "new", "release_tag": "v1", "downloads": 4}}, "now")
        self.assertEqual(sum(v["max_downloads_observed"] for v in history["assets"].values()), 16)
        self.assertEqual(history["assets"]["1"]["last_seen_at"], "before")

    def test_history_validation_and_coverage(self):
        history = {"clones": {"2026-09-01": {"count": 1, "uniques": 1}}}
        m.merge_traffic(history, "clones", {"clones": [{"timestamp": "2026-10-10T00:00:00Z", "count": 8, "uniques": 5}]})
        self.assertIn("2026-09-01", history["clones"])
        with self.assertRaises(ValueError):
            m.merge_traffic(history, "clones", {"clones": {}})
        with self.assertRaises(ValueError):
            m.popular([{"referrer": "x", "count": -1, "uniques": 1}], "referrers")
        series = {(NOW.date() - timedelta(days=i)).isoformat(): {"count": 4, "uniques": 2} for i in range(1, 31)}
        result = m.summarize_traffic({"clones": series, "views": series}, NOW.date())
        self.assertEqual(result["clones"]["last_30_complete_days"]["count"], 120)
        self.assertEqual(result["clones"]["growth_vs_previous_7_days_percent"], 0)
        self.assertNotIn("uniques", result["clones"]["last_7_complete_days"])
        del series[(NOW.date() - timedelta(days=2)).isoformat()]
        self.assertIsNone(m.summarize_traffic({"clones": series, "views": series}, NOW.date())["clones"]["growth_vs_previous_7_days_percent"])

    def test_collection_schema_and_missing_token(self):
        with tempfile.TemporaryDirectory() as temp, patch.object(m, "github_json", side_effect=api):
            out = Path(temp)
            first = m.collect("vela-desktop/vela", out, "release", "traffic", NOW)
            self.assertIsNone(first["downloads_since_previous_snapshot"])
            s = json.loads((out / "data/summary.json").read_text())
            self.assertEqual(s["schema_version"], 1)
            self.assertEqual(s["top_referrers"][0]["referrer"], "Google")
            self.assertEqual(s["popular_paths"][0]["path"], "/vela-desktop/vela")
            self.assertTrue((out / "data/discovery-snapshots/2026-10-10.json").exists())
            m.collect("vela-desktop/vela", out, "release", "", NOW + timedelta(days=1))
            s = json.loads((out / "data/summary.json").read_text())
            self.assertIsNone(s["traffic"])
            self.assertIsNone(s["top_referrers"])
            self.assertEqual(s["sources"]["traffic"]["status"], "not_configured")
            self.assertEqual(s["releases"]["downloads_since_previous_daily_snapshot"], 0)

    def test_failed_api_does_not_write_partial_data(self):
        def fail(url, token):
            if url.endswith("/traffic/popular/paths"):
                raise RuntimeError("API unavailable")
            return api(url, token)
        with tempfile.TemporaryDirectory() as temp, patch.object(m, "github_json", side_effect=fail):
            with self.assertRaises(RuntimeError):
                m.collect("vela-desktop/vela", Path(temp), "", "token", NOW)
            self.assertFalse((Path(temp) / "data").exists())

    def test_auth_fails_fast_and_5xx_retries(self):
        def denied(request, timeout):
            raise urllib.error.HTTPError(request.full_url, 403, "Forbidden", {}, BytesIO())
        with patch.object(m.urllib.request, "urlopen", side_effect=denied) as mock:
            with self.assertRaisesRegex(RuntimeError, "HTTP 403"):
                m.github_json("https://api.github.com/repos/a/b", "token")
            self.assertEqual(mock.call_count, 1)
        class Response:
            def __enter__(self): return BytesIO(b'{"ok": true}')
            def __exit__(self, *args): return None
        def flaky(request, timeout):
            if mock.call_count == 1:
                raise urllib.error.HTTPError(request.full_url, 502, "Bad Gateway", {}, BytesIO())
            return Response()
        with patch.object(m.urllib.request, "urlopen", side_effect=flaky) as mock:
            with patch.object(m.time, "sleep") as sleep:
                self.assertEqual(m.github_json("https://api.github.com/repos/a/b", ""), {"ok": True})
                self.assertEqual(mock.call_count, 2)
                sleep.assert_called_once()

if __name__ == "__main__":
    unittest.main()
