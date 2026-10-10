# GitHub distribution metrics

Daily (03:17 UTC) and manually triggered GitHub-side aggregate statistics, with no
Vela runtime telemetry. Pull requests run collector tests with read-only access.

## Sources and interpretation

The collector archives GitHub clone and page-view traffic by UTC day, plus
top-ten referrers and popular paths. Traffic API daily data goes back at most
14 days. Rankings are **rolling 14-day top-ten aggregates**, NOT daily values;
do not add overlapping rankings or daily uniques to estimate distinct people.

GitHub Release download counters are tracked per release and asset ID.
`downloads_current_assets` is the sum of existing asset counters;
`downloads_observed_maxima` preserves last-known maxima for deleted assets.
Neither is an exact lifetime unique-user count. Daily snapshot deltas cover
the interval between observed snapshots, not necessarily 24 hours, and new
assets can have downloads predating their first observation.

No source archive downloads, Actions artifact downloads, local builds,
package-manager installs, Vela starts or crash statistics are collected.

## Public output on the separate metrics branch

| File | Meaning |
| --- | --- |
| data/latest.json | Latest totals and data source status |
| data/summary.json | Versioned, dashboard-friendly traffic and release summary |
| data/traffic-daily.json | Per-day clone/view history |
| data/referrers.json | Latest rolling-14-day top-ten referral sources |
| data/popular-paths.json | Latest rolling-14-day top-ten paths |
| data/discovery-snapshots/YYYY-MM-DD.json | Archived overlapping top-ten windows |
| data/release-snapshots/YYYY-MM-DD.json | Individual asset counters by day |
| data/release-asset-history.json | Highest observed counter for each asset ID |

JSON documents include `schema_version: 1` and collection timestamps.
The summary includes 7-day, preceding 7-day and 30-day traffic windows,
excluding today's incomplete UTC day. Every window includes `observed_days`
and `complete`. When days are missing, a count is partial, never interpreted
as a complete period. Growth is null unless both seven-day windows are complete
and the previous count is nonzero. Unique visitors are not summed across days.

## Setup after merge

1. Create a fine-grained PAT scoped to **vela-desktop/vela** with repository
   **Administration: Read-only** (organization approval may be necessary).
2. Store it in Actions secrets as `VELA_TRAFFIC_TOKEN`.
3. Allow `contents: write` to the generated `metrics` branch in applicable rulesets.
4. Trigger Actions → Distribution metrics → Run workflow to record the baseline.

Only the built-in `GITHUB_TOKEN` writes data. Without the PAT, release metrics
are still collected but traffic is marked `not_configured` rather than zero;
previous traffic history remains archived. A configured but invalid PAT fails
the collection instead of committing misleading partial results.

## Validation

Run `python3 -m unittest discover -s scripts/tests -p 'test_github_distribution_metrics.py'`.
The dedicated PR workflow runs the same tests, covering mocked API responses,
retry on transient failures, payload validation, missing token, historical
assets and incomplete windows.

GitHub provides traffic history only for approximately 14 days. Monitor workflow
health and timestamps; inactivity can disable scheduled workflows on public repos.
