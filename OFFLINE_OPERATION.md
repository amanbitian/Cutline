# Offline operation contract

Cutline is a desktop editor whose complete editing workflow runs without an internet connection. Opening and saving projects, ingest, playback, proxies, effects, colour, audio, captions, recovery, interchange and export must use local code and locally available assets.

## Runtime boundary

- SQLite project packages, journals, snapshots and caches are local files.
- FFmpeg is an in-process media provider shipped with the application package. Proxy creation and export do not call a web API.
- AI features must have a bundled local provider or an installable offline model pack. A model download may be offered during installation, but an account or network connection cannot be required after installation.
- The old `mcp-server.js` browser prototype is not part of the native application and is not a product runtime dependency.
- Update checks, telemetry, stock assets and remote model providers are outside the core editor and must be opt-in modules if they are ever added.

## Collaboration

Single-user editing never starts or contacts a server. Future collaboration is optional and should begin with shared-folder packages or an explicitly configured LAN peer. Any hosted review or synchronization service is an add-on: losing it must not block local editing, saving, recovery or export.

## Packaging requirement

Release packages must include the native executable, Qt runtime, FFmpeg libraries, vendored SQLite, built-in effects and required local model/runtime assets for the selected edition. CI should include a networking-disabled end-to-end run that imports media, creates a proxy, edits, saves, reopens and exports.
