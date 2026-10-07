# Cutline MCP connection

Cutline exposes a local Model Context Protocol (MCP) server so an LLM can inspect the prototype project and make constrained, validated editorial, marker, caption, transform, color-grade, and export-queue commands. It uses no external package: Node.js 24+ is enough.

The current MCP server controls only the browser/Node prototype (`project-state.json`), which is a superseded UX reference. It does **not** control the native SQLite core, the 69-command bus, the compiler, the playback engine or export. Nothing in the native core depends on it. A native MCP/IPC adapter must submit typed command envelopes through `CommandService`; it must never write the SQLite database directly. The native media and editing path now exists (decode, compose, mix, export, undo), so the native adapter is unblocked: it should translate MCP tool calls into `CommandEnvelope`s and submit them through `ProjectStore::Execute`, supplying `base_revision` from `CurrentRevision()` so a stale agent is rejected rather than overwriting. It is not built, and the rule stands that an adapter must never touch the database directly.

## Start the editor bridge

```powershell
node mcp-server.js --http-port 4173
```

Then open `http://127.0.0.1:4173` in a browser. The browser UI and MCP server both read/write `project-state.json`; browser clients poll for remote updates, so LLM changes appear in the timeline.

## Connect an MCP client

Add a local stdio server using the following shape in your MCP-client configuration. Replace the path with the absolute location of this repository.

```json
{
  "mcpServers": {
    "cutline": {
      "command": "node",
      "args": ["F:\\AAA Project\\video_editor\\mcp-server.js", "--http-port", "4173"]
    }
  }
}
```

The server reserves standard output for MCP JSON-RPC. Its HTTP preview status is emitted on standard error, which avoids corrupting MCP communication.

## Tools

| Tool | Access | Purpose |
|---|---|---|
| `cutline_get_project` | Read-only | Project, media, tracks, markers, captions, and jobs |
| `cutline_get_timeline` | Read-only | Sequence and clip IDs for subsequent operations |
| `cutline_undo` / `cutline_redo` | Write | Travel the validated project command history |
| `cutline_create_snapshot` | Write | Create an on-disk recovery checkpoint |
| `cutline_move_clip` | Write | Move a clip to a compatible track/position |
| `cutline_trim_clip` | Write | Change prototype clip position/duration |
| `cutline_add_track` | Write | Add an audio or video track |
| `cutline_add_clip` | Write | Add registered media to a compatible track |
| `cutline_split_clip` | Write | Split a selected clip at a valid timeline position |
| `cutline_ripple_delete` | Write | Delete a clip and close its track gap |
| `cutline_set_color_grade` | Write | Save non-destructive clip grade parameters: exposure, contrast, saturation, temperature, tint, highlights, shadows, LUT name |
| `cutline_set_transform` | Write | Save x/y, scale, rotation, or opacity values |
| `cutline_add_marker` | Write | Create a named frame marker |
| `cutline_add_caption` | Write | Create a timed caption |
| `cutline_queue_export` | Write | Record a render/export job |

All writes are input-validated, update the project revision, append a durable command record to `project-journal.ndjson`, maintain bounded undo/redo history, and create automatic checkpoints every 25 commands. Browser-originated commands include their base revision and are rejected if the project changed first. The tool surface intentionally does not expose arbitrary file-system, shell, raw SQL, or project-document writes.

Timeline responses include canonical JSON-safe rational `startTime` and `durationTime` values on clips. They are the first exact-time project fields; the browser timeline still uses display coordinates and no source VFR/audio clock exists yet.

## Example LLM workflow

1. Call `cutline_get_timeline`.
2. Identify `clip-lake` on `V1`.
3. Call `cutline_set_color_grade` with `{"clipId":"clip-lake","grade":{"exposure":0.35,"contrast":12,"saturation":108}}`.
4. Call `cutline_add_marker` with `{"frame":296,"name":"Grade pass"}`.
5. Review the structured tool result and, if desired, call `cutline_queue_export`.

## Security model

Run this MCP server locally. It binds HTTP only to `127.0.0.1` and does not expose a public network port. Treat any LLM/client that is granted this server as having permission to make the documented project edits. Production deployment should additionally add client authentication, user/workspace authorization, confirmation rules for exports or cloud uploads, project locking, audit retention, and process-level sandboxing.

OpenAI’s current MCP guidance supports remote MCP servers and local/private servers through Secure MCP Tunnel; it also emphasizes tool approval and trusting only reviewed servers. See the [official MCP server guide](https://developers.openai.com/api/docs/guides/tools-connectors-mcp).
