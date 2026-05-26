"""Tool schemas exposed to the Godot AI agent."""

from typing import Any

from pydantic import BaseModel, Field

# ---------------------------------------------------------------------------
# Editor-executed tools (run inside the Godot editor via ai_assistant_dock)
# ---------------------------------------------------------------------------


class ReadFile(BaseModel):
    """Read a project file from a Godot res:// path."""

    path: str = Field(description="Godot path, e.g. res://player.gd")


class WriteFile(BaseModel):
    """Write or overwrite a text file (.gd, .tscn, .tres, .cfg, .md, etc.)."""

    path: str = Field(description="Godot path, e.g. res://player.gd")
    contents: str = Field(description="Full file contents")


class WriteBinaryFile(BaseModel):
    """Write raw bytes to a file from a base64-encoded payload."""

    path: str = Field(description="Godot path, e.g. res://assets/sprite.png")
    data_base64: str = Field(description="Base64-encoded file bytes")


class DeleteFile(BaseModel):
    """Delete a file from the project."""

    path: str = Field(description="Godot path to delete")


class MakeDir(BaseModel):
    """Create a directory (and parents) under res://."""

    path: str = Field(description="Directory path, e.g. res://assets/sprites")


class ListDir(BaseModel):
    """List files and folders under a Godot path."""

    path: str = Field(default="res://", description="Directory path")


class DownloadUrl(BaseModel):
    """Download a file from an HTTP(S) URL into the project."""

    url: str = Field(description="Direct download URL")
    path: str = Field(description="Destination res:// path")


class ExtractZip(BaseModel):
    """Extract a .zip archive into a project directory."""

    zip_path: str = Field(description="res:// path to the .zip file")
    target_dir: str = Field(description="res:// directory to extract into")


class OpenScene(BaseModel):
    """Open a scene in the Godot editor."""

    path: str = Field(description="Scene path, e.g. res://main.tscn")


class SaveScene(BaseModel):
    """Save the currently edited scene to disk."""

    path: str = Field(
        default="",
        description="Optional res:// path. Empty uses the scene's current path.",
    )


class GetSceneTree(BaseModel):
    """Inspect the node tree of the currently edited scene."""


class GetOpenScriptText(BaseModel):
    """Read the currently open script in the editor."""


class GetProjectInfo(BaseModel):
    """Read key project settings, autoloads, and editor state."""


class SetProjectSetting(BaseModel):
    """Set a project.godot setting and save."""

    key: str = Field(description="ProjectSettings key, e.g. application/config/name")
    value: str = Field(
        description="JSON-encoded value (string, number, bool, array, object)"
    )


class SetMainScene(BaseModel):
    """Set the project's main scene."""

    path: str = Field(description="Main scene res:// path")


class SetAutoload(BaseModel):
    """Register or update an autoload singleton."""

    name: str = Field(description="Autoload name, e.g. GameManager")
    path: str = Field(description="Script/scene res:// path")
    singleton: bool = Field(default=True, description="Whether this is a singleton")


class ScanFilesystem(BaseModel):
    """Trigger a filesystem scan so new/changed assets are imported."""


class CreateSvg(BaseModel):
    """Create or overwrite a custom SVG icon/sprite asset."""

    path: str = Field(description="res:// path ending in .svg, e.g. res://icon.svg")
    contents: str = Field(
        description="Full SVG XML. Must include an <svg> root with xmlns='http://www.w3.org/2000/svg'."
    )
    width: int = Field(default=0, description="Optional width attribute for the <svg> root (0 = omit)")
    height: int = Field(default=0, description="Optional height attribute for the <svg> root (0 = omit)")


class RunProject(BaseModel):
    """Run the game headless to detect runtime/script errors."""

    scene_path: str = Field(
        default="",
        description="Optional res:// scene to run. Empty uses the project main scene.",
    )
    quit_after_frames: int = Field(
        default=60,
        description="Stop after this many frames (approx. 1s at 60 FPS).",
    )


class VerifyDeliverables(BaseModel):
    """Check that expected project files/scenes exist before finishing a task."""

    paths: list[str] = Field(
        default_factory=list,
        description="res:// paths that must exist (files or directories).",
    )
    require_main_scene: bool = Field(
        default=False,
        description="If true, fail when application/run/main_scene is unset.",
    )


# ---------------------------------------------------------------------------
# Backend-executed tools (run in the Python sidecar)
# ---------------------------------------------------------------------------


class SearchAssetLibrary(BaseModel):
    """Search the official Godot Asset Library for sprites, audio, templates, etc."""

    query: str = Field(description="Search keywords, e.g. 'platformer sprite'")
    page: int = Field(default=0, description="Results page (0-based)")
    category: int = Field(
        default=0, description="Category id (0 = all). Use get_asset_library_categories."
    )


class GetAssetLibraryCategories(BaseModel):
    """List Godot Asset Library categories."""


class GetAssetDetails(BaseModel):
    """Get download URL and metadata for a Godot Asset Library asset."""

    asset_id: int = Field(description="Asset id from search_asset_library results")


class VerifyTaskCompletion(BaseModel):
    """Evaluate whether the user's request appears complete given verification results."""

    user_goal: str = Field(description="Original user request being verified")
    deliverables: list[str] = Field(
        default_factory=list,
        description="Files, scenes, or features that should exist",
    )
    run_result_json: str = Field(
        default="{}",
        description="JSON object from run_project (fields: ok, errors, output, exit_code, main_scene, ...).",
    )
    file_checks_json: str = Field(
        default="[]",
        description="JSON array from verify_deliverables 'checks' field [{path, exists}, ...].",
    )


EDITOR_TOOL_SPECS = {
    "read_file": ReadFile,
    "write_file": WriteFile,
    "write_binary_file": WriteBinaryFile,
    "delete_file": DeleteFile,
    "make_dir": MakeDir,
    "list_dir": ListDir,
    "download_url": DownloadUrl,
    "extract_zip": ExtractZip,
    "open_scene": OpenScene,
    "save_scene": SaveScene,
    "get_scene_tree": GetSceneTree,
    "get_open_script_text": GetOpenScriptText,
    "get_project_info": GetProjectInfo,
    "set_project_setting": SetProjectSetting,
    "set_main_scene": SetMainScene,
    "set_autoload": SetAutoload,
    "scan_filesystem": ScanFilesystem,
    "create_svg": CreateSvg,
    "run_project": RunProject,
    "verify_deliverables": VerifyDeliverables,
}

BACKEND_TOOL_SPECS = {
    "search_asset_library": SearchAssetLibrary,
    "get_asset_library_categories": GetAssetLibraryCategories,
    "get_asset_details": GetAssetDetails,
    "verify_task_completion": VerifyTaskCompletion,
}

TOOL_SPECS = {**EDITOR_TOOL_SPECS, **BACKEND_TOOL_SPECS}

BACKEND_TOOLS = set(BACKEND_TOOL_SPECS.keys())
