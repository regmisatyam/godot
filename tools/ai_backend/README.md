# Godot AI Backend

Python sidecar for the built-in **AI Assistant** editor dock. It runs a Cerebras-backed agent and exposes HTTP endpoints the C++ dock calls.

## Setup

```bash
cd tools/ai_backend
python3 -m venv .venv
source .venv/bin/activate
pip install -e .
export CEREBRAS_API_KEY=your_key_here
```

Optional: override the default model (`gpt-oss-120b`):

```bash
export CEREBRAS_MODEL=gpt-oss-120b
```

## Run

```bash
ai-backend
```

The server listens on `http://127.0.0.1:8765`.

## Endpoints

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/health` | Connectivity check |
| POST | `/session/restore` | Restore a saved chat session on the backend |
| POST | `/chat` | Send a prompt (optionally with images) or continue after tool execution |
| POST | `/tool_result` | Return editor tool output to the agent |

## Chat history

Each Godot project stores AI chats under `.godot/ai_assistant/`:

- `chats.json` — chat titles, messages, and session IDs
- `images/` — image attachments referenced by chat messages

Use **New Chat** in the dock to start a fresh conversation. Switch chats from the dropdown to resume prior context (the dock restores backend state automatically).

## Image attachments

Click **Attach Image** to include PNG, JPEG, WebP, GIF, or BMP files with a message. Images appear in the transcript and are persisted with the chat. The backend notes attachments in the prompt; full pixel-level vision depends on the configured model.

## Build the editor

From the Godot repo root:

```bash
scons platform=macos arch=arm64 target=editor dev_build=yes -j8
bin/godot.macos.editor.dev.arm64
```

Open a project, start the backend, then use the **AI Assistant** dock on the left.

## Tool flow

1. Dock POSTs `/chat` with the user prompt and project context.
2. Backend tools (asset search) run in the sidecar; editor tools run in Godot.
3. Cerebras may return tool calls for files, scenes, downloads, project settings, etc.
4. The dock executes editor tools and POSTs each result to `/tool_result`.
5. The dock POSTs `/chat` again with an empty prompt so the agent continues.
6. When no more tools are needed, the agent's text reply appears in the dock.

## Agent capabilities

| Area | Tools |
|------|-------|
| Files | `read_file`, `write_file`, `write_binary_file`, `delete_file`, `make_dir`, `list_dir` |
| Assets | `search_asset_library`, `get_asset_details`, `download_url`, `extract_zip`, `scan_filesystem` |
| Scenes | `open_scene`, `save_scene`, `get_scene_tree`, `get_open_script_text` |
| Project | `get_project_info`, `set_project_setting`, `set_main_scene`, `set_autoload` |
| SVG | `create_svg` — write custom SVG icons/sprites (auto-imported as textures) |
| Verify & run | `verify_deliverables`, `run_project`, `verify_task_completion` |

## Completion verification

Before finishing a game-building task, the agent must:

1. `verify_deliverables` — confirm created files/scenes exist
2. `run_project` — headless run to catch script/runtime errors
3. `verify_task_completion` — evaluate whether the user's goal is met

If errors appear, the agent fixes them and re-runs (up to 3 cycles).
