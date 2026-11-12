# Access UI (9e)

The home status card highlights current access state; errors are red, a persisted simulated grant green, a stranger amber. This remains simulated authorization, not proof of physical passage.

The record page replaces the previous empty events placeholder with the latest 50 passage metadata rows. Four rows per page, 64-pixel or larger touch targets, bounded previous/next navigation and a separate metadata detail view. Times are displayed in the board local timezone. Detail shows historical name, score, linked/deleted person and whether a snapshot was saved; image viewing is not included in this increment. Older records remain available through --passes.

PassHistory owns read-only SQLite access in a worker. It reconnects and refreshes every two seconds, publishes one bounded result and joins on stop. UI never performs SQLite reads. Missing databases are not created, old versions are not migrated by this reader. Loading, empty and failure states differ; failures hide stale rows. Selected details are retained by database ID or closed when unavailable. No editing/deletion controls are exposed.

The existing recording pipeline is retained; this page currently lists passage records rather than security video events. Camera connection stability and the deferred manual stranger/fault tests remain open.
