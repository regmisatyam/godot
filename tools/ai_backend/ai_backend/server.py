"""FastAPI sidecar for the Godot AI assistant dock."""

from __future__ import annotations

import os
from typing import Any

import uvicorn
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

from .graph import (
    append_tool_message,
    get_model,
    last_assistant_text,
    make_user_message,
    restore_session_messages,
    run_agent_until_editor_tools,
)

app = FastAPI(title="Godot AI Backend", version="0.1.0")


class SessionState:
    def __init__(self) -> None:
        self.messages: list[dict[str, Any]] = []
        self.pending_tool_calls: list[dict[str, Any]] = []


SESSIONS: dict[str, SessionState] = {}


class ImageAttachment(BaseModel):
    filename: str = "image.png"
    mime_type: str = "image/png"
    data_base64: str = ""


class ChatRequest(BaseModel):
    session_id: str
    prompt: str = ""
    context: dict[str, Any] = Field(default_factory=dict)
    images: list[ImageAttachment] = Field(default_factory=list)


class ToolResultRequest(BaseModel):
    session_id: str
    tool_call_id: str
    result: dict[str, Any] = Field(default_factory=dict)


class SessionRestoreRequest(BaseModel):
    session_id: str
    messages: list[dict[str, Any]] = Field(default_factory=list)


def get_session(session_id: str) -> SessionState:
    if session_id not in SESSIONS:
        SESSIONS[session_id] = SessionState()
    return SESSIONS[session_id]


@app.get("/health")
async def health() -> dict[str, Any]:
    has_key = bool(os.environ.get("CEREBRAS_API_KEY"))
    return {
        "ok": True,
        "provider": "cerebras",
        "model": get_model(),
        "cerebras_api_key_set": has_key,
    }


@app.post("/session/restore")
async def restore_session(request: SessionRestoreRequest) -> dict[str, Any]:
    session = SessionState()
    session.messages = restore_session_messages(request.messages)
    SESSIONS[request.session_id] = session
    return {"ok": True, "message_count": len(session.messages)}


@app.post("/chat")
async def chat(request: ChatRequest) -> dict[str, Any]:
    if not os.environ.get("CEREBRAS_API_KEY"):
        raise HTTPException(
            status_code=503,
            detail="CEREBRAS_API_KEY is not set. Export it before starting ai-backend.",
        )

    session = get_session(request.session_id)

    if request.prompt or request.images:
        image_payload = [image.model_dump() for image in request.images]
        session.messages.append(
            make_user_message(request.prompt, request.context, image_payload)
        )

    result = await run_agent_until_editor_tools(session.messages)
    pending = result.get("tool_calls") or []
    session.pending_tool_calls = pending

    if pending:
        return {"text": "", "tool_calls": pending}

    return {"text": last_assistant_text(session.messages), "tool_calls": []}


@app.post("/tool_result")
async def tool_result(request: ToolResultRequest) -> dict[str, Any]:
    session = SESSIONS.get(request.session_id)
    if session is None:
        raise HTTPException(status_code=404, detail="Unknown session")

    append_tool_message(session.messages, request.tool_call_id, request.result)
    session.pending_tool_calls = [
        call
        for call in session.pending_tool_calls
        if call.get("id") != request.tool_call_id
    ]

    return {
        "ok": True,
        "remaining_tool_calls": len(session.pending_tool_calls),
    }


def main() -> None:
    uvicorn.run(
        "ai_backend.server:app",
        host="127.0.0.1",
        port=8765,
        log_level="info",
    )


if __name__ == "__main__":
    main()
