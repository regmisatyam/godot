/**************************************************************************/
/*  ai_assistant_dock.h                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "editor/docks/editor_dock.h"

class Button;
class EditorFileDialog;
class HBoxContainer;
class HTTPRequest;
class LineEdit;
class OptionButton;
class RichTextLabel;
class TextEdit;

class AIAssistantDock : public EditorDock {
	GDCLASS(AIAssistantDock, EditorDock);

	RichTextLabel *transcript = nullptr;
	TextEdit *input = nullptr;
	Button *send_button = nullptr;
	Button *stop_button = nullptr;
	Button *new_chat_button = nullptr;
	Button *attach_button = nullptr;
	LineEdit *status_line = nullptr;
	OptionButton *chat_selector = nullptr;
	HBoxContainer *attachment_preview = nullptr;
	EditorFileDialog *image_dialog = nullptr;

	HTTPRequest *http_health = nullptr;
	HTTPRequest *http_chat = nullptr;
	HTTPRequest *http_tool = nullptr;
	HTTPRequest *http_session = nullptr;

	String backend_url = "http://127.0.0.1:8765";
	String session_id;
	String active_chat_id;
	Array chats;
	Array pending_images;
	bool busy = false;
	int pending_tool_results = 0;

	void _on_send_pressed();
	void _on_stop_pressed();
	void _on_new_chat_pressed();
	void _on_attach_pressed();
	void _on_image_selected(const String &p_path);
	void _on_chat_selected(int p_index);
	void _on_input_text_changed();
	void _on_health_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _on_chat_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _on_tool_result_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	void _on_session_restore_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body);

	void _dispatch_tool_calls(const Array &p_calls);
	String _execute_tool(const Dictionary &p_call);
	String _tool_read_file(const String &p_path);
	String _tool_write_file(const String &p_path, const String &p_contents);
	String _tool_write_binary_file(const String &p_path, const String &p_data_base64);
	String _tool_delete_file(const String &p_path);
	String _tool_make_dir(const String &p_path);
	String _tool_list_dir(const String &p_path);
	String _tool_download_url(const String &p_url, const String &p_path);
	String _tool_extract_zip(const String &p_zip_path, const String &p_target_dir);
	String _tool_open_scene(const String &p_path);
	String _tool_save_scene(const String &p_path);
	String _tool_get_scene_tree();
	String _tool_get_open_script_text();
	String _tool_get_project_info();
	String _tool_set_project_setting(const String &p_key, const String &p_value_json);
	String _tool_set_main_scene(const String &p_path);
	String _tool_set_autoload(const String &p_name, const String &p_path, bool p_singleton);
	String _tool_scan_filesystem();
	String _tool_create_svg(const String &p_path, const String &p_contents, int p_width, int p_height);
	String _tool_run_project(const String &p_scene_path, int p_quit_after_frames);
	String _tool_verify_deliverables(const Array &p_paths, bool p_require_main_scene);
	Array _extract_run_errors(const String &p_output) const;
	Dictionary _node_to_dict(Node *p_node) const;
	PackedByteArray _http_download_sync(const String &p_url) const;
	void _continue_chat();
	void _post_tool_result(const String &p_tool_call_id, const String &p_result);

	void _append_user(const String &p_msg, const Array &p_images = Array());
	void _append_assistant(const String &p_msg);
	void _append_system(const String &p_msg);
	void _append_images_to_transcript(const Array &p_images);
	String _escape_bbcode(const String &p_text) const;
	String _format_inline_markdown(const String &p_text) const;
	String _preprocess_mdx(const String &p_text) const;
	String _markdown_to_bbcode(const String &p_markdown) const;
	void _set_busy(bool p_busy);
	void _update_send_enabled();
	void _refresh_attachment_preview();
	Dictionary _gather_project_context() const;

	String _generate_id() const;
	String _get_storage_dir() const;
	String _get_images_dir() const;
	String _get_chats_file() const;
	String _mime_type_for_path(const String &p_path) const;
	String _save_image_attachment(const String &p_source_path, const String &p_filename);
	String _read_file_base64(const String &p_path) const;
	void _load_chats();
	void _save_chats();
	Dictionary _get_active_chat() const;
	void _set_active_chat(const Dictionary &p_chat);
	int _find_chat_index(const String &p_chat_id) const;
	void _create_new_chat(bool p_make_active = true);
	void _switch_to_chat(const String &p_chat_id);
	void _refresh_chat_selector();
	void _rebuild_transcript();
	void _record_message(const String &p_role, const String &p_text, const Array &p_images = Array());
	void _restore_backend_session();
	Array _build_restore_messages() const;
	Array _build_image_payload(const Array &p_images) const;

protected:
	void _notification(int p_what);

public:
	AIAssistantDock();
};
