"""Cerebras-backed agent loop for the Godot AI assistant."""

from __future__ import annotations

import json
import os
import uuid
from typing import Any

from cerebras.cloud.sdk import AsyncCerebras

from .backend_tools import execute_backend_tool
from .tools import BACKEND_TOOLS, TOOL_SPECS

SYSTEM_PROMPT = """You are an expert autonomous Godot 4.6 game-development agent embedded in a custom editor.
You can modify every part of a Godot project: scripts, scenes, resources, project settings, autoloads, imports, and assets.

## Workflow
1. Inspect the project (`get_project_info`, `list_dir`, `read_file`, `get_scene_tree`).
2. Plan the feature or fix.
3. Acquire assets when needed (`search_asset_library` → `get_asset_details` → `download_url` → `extract_zip` → `scan_filesystem`).
4. Create or edit scenes/scripts/resources with `write_file` (`.tscn`, `.gd`, `.tres` are text).
5. Create custom vector art with `create_svg` (icons, simple sprites, UI shapes). Godot imports SVG as textures.
6. Wire up gameplay: CharacterBody2D/3D, CollisionShape2D/3D, Sprite2D/MeshInstance3D, Area2D, animations, input, autoloads.
7. Set `set_main_scene`, save scenes.

## Completion verification (MANDATORY before telling the user you are done)
For any task that builds or changes a playable game:
1. Call `verify_deliverables` with every important `res://` path you created (and `require_main_scene=true` if the game should run).
2. Call `run_project` headless to catch script/runtime errors.
3. Call `verify_task_completion` with the original user goal, deliverables list, `run_result_json` (stringified run_project output), and `file_checks_json` (stringified verify_deliverables checks).
4. If `complete` is false or `run_project` reports errors: diagnose, fix files/scenes, then repeat steps 1–3 (up to 3 fix cycles).
5. Only give a final success summary when verification passes OR the user asked for something that cannot be run (e.g. docs only).

## Auto-fix on run errors
When `run_project` returns errors, read the output carefully, fix the root cause (missing nodes, typos, wrong paths, parse errors), re-run `scan_filesystem` if assets changed, and test again. Do not stop at the first failure.

## Custom SVG
Use `create_svg` for simple vector graphics. Example root:
`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64">...</svg>`
Reference the imported SVG in scenes via `res://your_icon.svg` on Sprite2D/TextureRect.

## Scene authoring (`.tscn`)
Write valid Godot 4 `.tscn` text directly. Typical 2D player:
- Root `CharacterBody2D` with child `Sprite2D` (texture) and `CollisionShape2D` (RectangleShape2D or CapsuleShape2D resource inline or via ext_resource).
- Add scripts with `[ext_resource type="Script" path="res://player.gd" id="1"]` and `script = ExtResource("1")`.
- Use `uid://` paths when present in existing project files; otherwise standard `ext_resource` paths work after import.

## Asset pipeline
- Search the Godot Asset Library for sprites, tilesets, audio, VFX, templates.
- Download zips with `download_url`, extract to e.g. `res://assets/`, run `scan_filesystem`.
- Prefer CC0 / permissive licenses. Mention license in summary.
- For external direct URLs (GitHub raw, itch.io CDN, etc.) use `download_url` too.

## Rules
- Always use `res://` paths.
- Prefer GDScript unless the user asks for C#.
- Use `write_file` for text formats; `write_binary_file` only when needed.
- After bulk file changes call `scan_filesystem`.
- Execute tools step by step; read results before continuing.
- Keep user-facing summaries concise and actionable.
"""


DEFAULT_MODEL = "gpt-oss-120b"


def get_model() -> str:
    return os.environ.get("CEREBRAS_MODEL", DEFAULT_MODEL)


def _normalize_strict_schema(node: Any) -> None:
    """Ensure object nodes satisfy Cerebras strict JSON-schema requirements."""
    if isinstance(node, dict):
        if node.get("type") == "object" and "anyOf" not in node and "properties" not in node:
            node["properties"] = {}
        if node.get("type") == "object":
            node["additionalProperties"] = False
            for prop in node.get("properties", {}).values():
                _normalize_strict_schema(prop)
        if node.get("type") == "array" and "items" in node:
            _normalize_strict_schema(node["items"])
        for key in ("$defs", "definitions", "items", "prefixItems"):
            if key in node and isinstance(node[key], dict):
                _normalize_strict_schema(node[key])
            elif key in node and isinstance(node[key], list):
                for item in node[key]:
                    _normalize_strict_schema(item)
        for defn in node.get("$defs", {}).values():
            _normalize_strict_schema(defn)
    elif isinstance(node, list):
        for item in node:
            _normalize_strict_schema(item)


def build_tool_definitions() -> list[dict[str, Any]]:
    tools: list[dict[str, Any]] = []
    for name, schema in TOOL_SPECS.items():
        parameters = schema.model_json_schema()
        parameters.pop("title", None)
        _normalize_strict_schema(parameters)
        tools.append(
            {
                "type": "function",
                "function": {
                    "name": name,
                    "description": (schema.__doc__ or name).strip(),
                    "strict": True,
                    "parameters": parameters,
                },
            }
        )
    return tools


def format_context(context: dict[str, Any]) -> str:
    return json.dumps(context, indent=2)


def make_user_message(
    prompt: str,
    context: dict[str, Any],
    images: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    if not prompt and not images:
        return {"role": "user", "content": "Continue using the tool results above."}

    parts: list[str] = []
    if context:
        parts.append(f"Project context:\n{format_context(context)}")
    if prompt:
        parts.append(f"User request:\n{prompt}")

    if images:
        attachment_lines = []
        for image in images:
            filename = image.get("filename") or "image"
            mime_type = image.get("mime_type") or "image/png"
            data_base64 = image.get("data_base64") or ""
            size_note = f"{len(data_base64)} base64 chars" if data_base64 else "no data"
            attachment_lines.append(f"- {filename} ({mime_type}, {size_note})")
        parts.append(
            "User attached image(s). Treat these as visual context:\n"
            + "\n".join(attachment_lines)
        )

    return {"role": "user", "content": "\n\n".join(parts)}


def restore_session_messages(messages: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Rebuild backend history from persisted user/assistant turns."""
    restored: list[dict[str, Any]] = []
    for message in messages:
        role = message.get("role")
        if role not in {"user", "assistant"}:
            continue

        content = message.get("content")
        if not isinstance(content, str):
            content = message.get("text") or ""

        if role == "user":
            images = message.get("images") or []
            restored.append(make_user_message(content, {}, images))
        else:
            restored.append({"role": "assistant", "content": content})
    return restored


def append_tool_message(
    messages: list[dict[str, Any]], tool_call_id: str, result: dict[str, Any]
) -> None:
    messages.append(
        {
            "role": "tool",
            "content": json.dumps(result),
            "tool_call_id": tool_call_id,
        }
    )


def last_assistant_text(messages: list[dict[str, Any]]) -> str:
    for message in reversed(messages):
        if message.get("role") == "assistant":
            content = message.get("content")
            if isinstance(content, str) and content.strip():
                return content
    return ""


async def run_agent_step(messages: list[dict[str, Any]]) -> dict[str, Any]:
    """Call Cerebras once and append the assistant turn to `messages`."""
    client = AsyncCerebras(api_key=os.environ.get("CEREBRAS_API_KEY"))

    response = await client.chat.completions.create(
        model=get_model(),
        messages=[{"role": "system", "content": SYSTEM_PROMPT}, *messages],
        tools=build_tool_definitions(),
        parallel_tool_calls=False,
        max_completion_tokens=8192,
        temperature=0.2,
        top_p=1,
        stream=False,
    )

    choice = response.choices[0].message
    assistant_message: dict[str, Any] = {
        "role": "assistant",
        "content": choice.content or "",
    }

    pending: list[dict[str, Any]] = []
    if choice.tool_calls:
        tool_calls_payload: list[dict[str, Any]] = []
        for tool_call in choice.tool_calls:
            tool_call_id = tool_call.id or str(uuid.uuid4())
            raw_arguments = tool_call.function.arguments
            if isinstance(raw_arguments, str):
                args = json.loads(raw_arguments) if raw_arguments else {}
            else:
                args = raw_arguments or {}

            pending.append(
                {
                    "id": tool_call_id,
                    "name": tool_call.function.name,
                    "args": args,
                }
            )
            tool_calls_payload.append(
                {
                    "id": tool_call_id,
                    "type": "function",
                    "function": {
                        "name": tool_call.function.name,
                        "arguments": raw_arguments
                        if isinstance(raw_arguments, str)
                        else json.dumps(raw_arguments),
                    },
                }
            )
        assistant_message["tool_calls"] = tool_calls_payload

    messages.append(assistant_message)
    return {"text": choice.content or "", "tool_calls": pending}


async def run_agent_until_editor_tools(
    messages: list[dict[str, Any]], *, max_backend_rounds: int = 12
) -> dict[str, Any]:
    """Run agent steps, executing backend tools inline until editor tools or text."""
    for _ in range(max_backend_rounds):
        result = await run_agent_step(messages)
        pending = result.get("tool_calls") or []
        if not pending:
            return {
                "text": last_assistant_text(messages) or result.get("text", ""),
                "tool_calls": [],
            }

        editor_calls: list[dict[str, Any]] = []
        for call in pending:
            name = call.get("name", "")
            if name in BACKEND_TOOLS:
                tool_result = await execute_backend_tool(name, call.get("args") or {})
                append_tool_message(messages, call.get("id", ""), tool_result)
            else:
                editor_calls.append(call)

        if editor_calls:
            return {"text": "", "tool_calls": editor_calls}

    return {
        "text": last_assistant_text(messages),
        "tool_calls": [],
        "warning": "backend tool loop limit reached",
    }
