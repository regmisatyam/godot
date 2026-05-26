"""Tools executed inside the Python sidecar (network / asset library)."""

from __future__ import annotations

import json
import os
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

ASSET_LIBRARY_API = os.environ.get(
    "GODOT_ASSET_LIBRARY_API",
    "https://godotengine.org/asset-library/api",
)
GODOT_VERSION_BRANCH = os.environ.get("GODOT_VERSION_BRANCH", "4.6")


def _http_get_json(url: str, timeout: float = 30.0) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "Godot-AI-Assistant/0.1"},
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = response.read()
    data = json.loads(payload.decode("utf-8"))
    if not isinstance(data, dict):
        return {"raw": data}
    return data


def search_asset_library(
    query: str, page: int = 0, category: int = 0
) -> dict[str, Any]:
    params: dict[str, str] = {
        "sort": "updated",
        "godot_version": GODOT_VERSION_BRANCH,
        "page": str(max(page, 0)),
    }
    if query.strip():
        params["filter"] = query.strip()
    if category > 0:
        params["category"] = str(category)
    params["support"] = "featured+community+testing"

    url = f"{ASSET_LIBRARY_API}/asset?{urllib.parse.urlencode(params)}"
    try:
        data = _http_get_json(url)
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        return {"ok": False, "error": str(exc)}

    results: list[dict[str, Any]] = []
    for item in data.get("result", []):
        results.append(
            {
                "asset_id": item.get("asset_id"),
                "title": item.get("title"),
                "author": item.get("author"),
                "category_id": item.get("category_id"),
                "license": item.get("cost"),
                "modified": item.get("modified"),
                "icon_url": item.get("icon_url"),
            }
        )

    return {
        "ok": True,
        "query": query,
        "page": data.get("page", page),
        "pages": data.get("pages", 1),
        "total": data.get("total", len(results)),
        "results": results,
    }


def get_asset_library_categories() -> dict[str, Any]:
    url = f"{ASSET_LIBRARY_API}/configure"
    try:
        data = _http_get_json(url)
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        return {"ok": False, "error": str(exc)}

    categories: list[dict[str, Any]] = []
    for item in data.get("categories", []):
        categories.append(
            {
                "id": item.get("id"),
                "name": item.get("name"),
            }
        )

    return {"ok": True, "categories": categories}


def get_asset_details(asset_id: int) -> dict[str, Any]:
    url = f"{ASSET_LIBRARY_API}/asset/{int(asset_id)}"
    try:
        data = _http_get_json(url)
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        return {"ok": False, "error": str(exc)}

    if not data.get("asset_id"):
        return {"ok": False, "error": "asset not found"}

    return {
        "ok": True,
        "asset_id": data.get("asset_id"),
        "title": data.get("title"),
        "author": data.get("author"),
        "description": data.get("description"),
        "version": data.get("version_string") or data.get("version"),
        "license": data.get("cost"),
        "download_url": data.get("download_url"),
        "download_hash": data.get("download_hash"),
        "browse_url": data.get("browse_url"),
        "category_id": data.get("category_id"),
    }


async def execute_backend_tool(name: str, args: dict[str, Any]) -> dict[str, Any]:
    if name == "search_asset_library":
        return search_asset_library(
            query=str(args.get("query", "")),
            page=int(args.get("page", 0)),
            category=int(args.get("category", 0)),
        )
    if name == "get_asset_library_categories":
        return get_asset_library_categories()
    if name == "get_asset_details":
        return get_asset_details(asset_id=int(args.get("asset_id", 0)))
    if name == "verify_task_completion":
        run_raw = args.get("run_result_json", args.get("run_result", "{}"))
        checks_raw = args.get("file_checks_json", args.get("file_checks", "[]"))
        if isinstance(run_raw, dict):
            run_result = run_raw
        else:
            try:
                run_result = json.loads(str(run_raw) or "{}")
            except json.JSONDecodeError:
                run_result = {}
        if isinstance(checks_raw, list):
            file_checks = checks_raw
        else:
            try:
                file_checks = json.loads(str(checks_raw) or "[]")
            except json.JSONDecodeError:
                file_checks = []
        return verify_task_completion(
            user_goal=str(args.get("user_goal", "")),
            deliverables=list(args.get("deliverables") or []),
            run_result=run_result if isinstance(run_result, dict) else {},
            file_checks=file_checks if isinstance(file_checks, list) else [],
        )
    return {"ok": False, "error": f"unknown backend tool: {name}"}


def verify_task_completion(
    user_goal: str,
    deliverables: list[str],
    run_result: dict[str, Any],
    file_checks: list[dict[str, Any]],
) -> dict[str, Any]:
    """Rule-based completion check combining file checks and run output."""
    issues: list[str] = []
    checks: list[dict[str, Any]] = []

    for entry in file_checks:
        path = str(entry.get("path", ""))
        exists = bool(entry.get("exists"))
        checks.append({"path": path, "exists": exists, "type": "file"})
        if path and not exists:
            issues.append(f"Missing expected path: {path}")

    main_scene = run_result.get("main_scene")
    if run_result.get("require_main_scene") and not main_scene:
        issues.append("Main scene is not configured.")

    run_errors = run_result.get("errors") or []
    if isinstance(run_errors, list):
        for err in run_errors:
            err_text = str(err).strip()
            if err_text:
                issues.append(err_text)

    if run_result.get("ran") and not run_result.get("ok", True):
        exit_code = run_result.get("exit_code")
        issues.append(f"Headless run failed (exit code {exit_code}).")

    output = str(run_result.get("output") or "")
    for marker in ("SCRIPT ERROR", "Parse Error", "Failed to load", "ERROR:"):
        if marker in output and marker not in " ".join(issues):
            for line in output.splitlines():
                if marker in line and line.strip() not in issues:
                    issues.append(line.strip())
                    break

    missing_deliverables = []
    known_paths = {str(c.get("path", "")) for c in file_checks if c.get("exists")}
    for item in deliverables:
        if item.startswith("res://") and item not in known_paths:
            missing_deliverables.append(item)
    if missing_deliverables:
        issues.append(
            "Deliverables not verified (run verify_deliverables): "
            + ", ".join(missing_deliverables)
        )

    complete = len(issues) == 0
    recommendation = (
        "Task appears complete. Summarise for the user."
        if complete
        else "Fix the listed issues, then run verify_deliverables and run_project again before finishing."
    )

    return {
        "ok": True,
        "complete": complete,
        "user_goal": user_goal,
        "issues": issues,
        "checks": checks,
        "recommendation": recommendation,
    }
