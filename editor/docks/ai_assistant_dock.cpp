/**************************************************************************/
/*  ai_assistant_dock.cpp                                                 */
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

#include "ai_assistant_dock.h"

#include "core/io/resource_uid.h"
#include "core/object/script_language.h"
#include "core/crypto/crypto.h"
#include "core/config/project_settings.h"
#include "core/core_bind.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/http_client.h"
#include "core/io/image.h"
#include "core/io/json.h"
#include "core/input/input_map.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/version.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/script/script_editor_plugin.h"
#include "editor/settings/editor_command_palette.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/texture_rect.h"
#include "scene/gui/text_edit.h"
#include "scene/main/http_request.h"
#include "scene/main/node.h"
#include "scene/resources/image_texture.h"

#ifdef MODULE_ZIP_ENABLED
#include "modules/zip/zip_reader.h"
#endif

void AIAssistantDock::_set_busy(bool p_busy) {
	busy = p_busy;
	_update_send_enabled();
	stop_button->set_disabled(!p_busy);
	chat_selector->set_disabled(p_busy);
	new_chat_button->set_disabled(p_busy);
	attach_button->set_disabled(p_busy);
	if (!p_busy) {
		status_line->set_text(TTR("Idle"));
	}
}

void AIAssistantDock::_update_send_enabled() {
	const bool has_content = !input->get_text().strip_edges().is_empty() || !pending_images.is_empty();
	send_button->set_disabled(busy || !has_content);
}

String AIAssistantDock::_generate_id() const {
	return itos(OS::get_singleton()->get_ticks_usec()) + "-" + itos(Math::rand());
}

String AIAssistantDock::_get_storage_dir() const {
	const String project_path = ProjectSettings::get_singleton()->get_resource_path();
	if (project_path.is_empty()) {
		return String();
	}
	return project_path.path_join(".godot/ai_assistant");
}

String AIAssistantDock::_get_images_dir() const {
	return _get_storage_dir().path_join("images");
}

String AIAssistantDock::_get_chats_file() const {
	return _get_storage_dir().path_join("chats.json");
}

String AIAssistantDock::_mime_type_for_path(const String &p_path) const {
	const String ext = p_path.get_extension().to_lower();
	if (ext == "jpg" || ext == "jpeg") {
		return "image/jpeg";
	}
	if (ext == "webp") {
		return "image/webp";
	}
	if (ext == "gif") {
		return "image/gif";
	}
	if (ext == "bmp") {
		return "image/bmp";
	}
	return "image/png";
}

String AIAssistantDock::_save_image_attachment(const String &p_source_path, const String &p_filename) {
	const String images_dir = _get_images_dir();
	if (images_dir.is_empty()) {
		return String();
	}

	Ref<DirAccess> dir = DirAccess::open("res://");
	if (dir.is_null()) {
		return String();
	}

	Error err = dir->make_dir_recursive(".godot/ai_assistant/images");
	if (err != OK && err != ERR_ALREADY_EXISTS) {
		return String();
	}

	const String ext = p_filename.get_extension().is_empty() ? "png" : p_filename.get_extension().to_lower();
	const String dest_path = images_dir.path_join(_generate_id() + "." + ext);
	const String source_global = ProjectSettings::get_singleton()->globalize_path(p_source_path);
	const String dest_global = ProjectSettings::get_singleton()->globalize_path(dest_path);

	err = DirAccess::copy_absolute(source_global, dest_global);
	if (err != OK) {
		return String();
	}

	return dest_path;
}

String AIAssistantDock::_read_file_base64(const String &p_path) const {
	Error err = OK;
	const Vector<uint8_t> data = FileAccess::get_file_as_bytes(p_path, &err);
	if (err != OK || data.is_empty()) {
		return String();
	}
	return CoreBind::Marshalls::get_singleton()->raw_to_base64(data);
}

int AIAssistantDock::_find_chat_index(const String &p_chat_id) const {
	for (int i = 0; i < chats.size(); i++) {
		if (chats[i].operator Dictionary().get("id", "") == p_chat_id) {
			return i;
		}
	}
	return -1;
}

Dictionary AIAssistantDock::_get_active_chat() const {
	const int index = _find_chat_index(active_chat_id);
	if (index == -1) {
		return Dictionary();
	}
	return chats[index];
}

void AIAssistantDock::_set_active_chat(const Dictionary &p_chat) {
	const int index = _find_chat_index(p_chat.get("id", ""));
	if (index == -1) {
		return;
	}
	chats[index] = p_chat;
}

void AIAssistantDock::_load_chats() {
	chats.clear();
	active_chat_id = "";

	const String chats_file = _get_chats_file();
	if (chats_file.is_empty() || !FileAccess::exists(chats_file)) {
		_create_new_chat(true);
		return;
	}

	Error err = OK;
	const String json_text = FileAccess::get_file_as_string(chats_file, &err);
	if (err != OK || json_text.is_empty()) {
		_create_new_chat(true);
		return;
	}

	Variant parsed = JSON::parse_string(json_text);
	if (parsed.get_type() != Variant::DICTIONARY) {
		_create_new_chat(true);
		return;
	}

	Dictionary data = parsed;
	if (data.has("chats")) {
		chats = data["chats"];
	}
	active_chat_id = data.get("active_chat_id", "");

	if (chats.is_empty()) {
		_create_new_chat(true);
		return;
	}

	if (_find_chat_index(active_chat_id) == -1) {
		active_chat_id = chats[0].operator Dictionary().get("id", "");
	}

	Dictionary active = _get_active_chat();
	session_id = active.get("session_id", _generate_id());
	_rebuild_transcript();
	_refresh_chat_selector();
	_restore_backend_session();
}

void AIAssistantDock::_save_chats() {
	const String storage_dir = _get_storage_dir();
	if (storage_dir.is_empty() || active_chat_id.is_empty()) {
		return;
	}

	Ref<DirAccess> dir = DirAccess::open("res://");
	if (dir.is_null()) {
		return;
	}
	dir->make_dir_recursive(".godot/ai_assistant");

	Dictionary data;
	data["version"] = 1;
	data["active_chat_id"] = active_chat_id;
	data["chats"] = chats;

	Ref<FileAccess> file = FileAccess::open(_get_chats_file(), FileAccess::WRITE);
	if (file.is_null()) {
		return;
	}
	file->store_string(JSON::stringify(data, "\t"));
	file->close();
}

void AIAssistantDock::_create_new_chat(bool p_make_active) {
	if (p_make_active && !active_chat_id.is_empty()) {
		_save_chats();
	}

	Dictionary chat;
	chat["id"] = _generate_id();
	chat["title"] = TTR("New chat");
	chat["session_id"] = _generate_id();
	chat["created_at"] = (int64_t)Time::get_singleton()->get_unix_time_from_system();
	chat["updated_at"] = chat["created_at"];
	chat["messages"] = Array();

	chats.push_back(chat);

	if (p_make_active) {
		active_chat_id = chat["id"];
		session_id = chat["session_id"];
		pending_images.clear();
		input->set_text("");
		_refresh_attachment_preview();
		transcript->clear();
		_refresh_chat_selector();
		_save_chats();
	}
}

void AIAssistantDock::_switch_to_chat(const String &p_chat_id) {
	if (p_chat_id.is_empty() || p_chat_id == active_chat_id) {
		return;
	}

	_save_chats();
	active_chat_id = p_chat_id;

	Dictionary active = _get_active_chat();
	session_id = active.get("session_id", _generate_id());
	pending_images.clear();
	input->set_text("");
	_refresh_attachment_preview();
	_rebuild_transcript();
	_refresh_chat_selector();
	_restore_backend_session();
	_save_chats();
}

void AIAssistantDock::_refresh_chat_selector() {
	chat_selector->set_block_signals(true);
	chat_selector->clear();
	for (int i = 0; i < chats.size(); i++) {
		Dictionary chat = chats[i];
		const String title = chat.get("title", TTR("Chat"));
		chat_selector->add_item(title);
		if (chat.get("id", "") == active_chat_id) {
			chat_selector->select(i);
		}
	}
	chat_selector->set_block_signals(false);
}

void AIAssistantDock::_rebuild_transcript() {
	transcript->clear();
	Dictionary active = _get_active_chat();
	if (active.is_empty()) {
		return;
	}

	Array messages = active.get("messages", Array());
	for (int i = 0; i < messages.size(); i++) {
		Dictionary message = messages[i];
		const String role = message.get("role", "");
		const String text = message.get("text", "");
		const Array images = message.get("images", Array());

		if (role == "user") {
			_append_user(text, images);
		} else if (role == "assistant") {
			_append_assistant(text);
		} else if (role == "system") {
			_append_system(text);
		}
	}
}

void AIAssistantDock::_record_message(const String &p_role, const String &p_text, const Array &p_images) {
	Dictionary active = _get_active_chat();
	if (active.is_empty()) {
		return;
	}

	Dictionary message;
	message["role"] = p_role;
	message["text"] = p_text;
	message["images"] = p_images;
	message["timestamp"] = (int64_t)Time::get_singleton()->get_unix_time_from_system();

	Array messages = active.get("messages", Array());
	messages.push_back(message);
	active["messages"] = messages;
	active["updated_at"] = message["timestamp"];

	if (p_role == "user" && active.get("title", "") == TTR("New chat")) {
		String title = p_text.strip_edges();
		if (title.is_empty() && !p_images.is_empty()) {
			Dictionary first_image = p_images[0];
			title = vformat(TTR("Image: %s"), String(first_image.get("filename", "image")));
		}
		if (title.length() > 48) {
			title = title.substr(0, 45) + "...";
		}
		if (!title.is_empty()) {
			active["title"] = title;
		}
	}

	_set_active_chat(active);
	_refresh_chat_selector();
	_save_chats();
}

Array AIAssistantDock::_build_restore_messages() const {
	Array restore;
	Dictionary active = _get_active_chat();
	if (active.is_empty()) {
		return restore;
	}

	Array messages = active.get("messages", Array());
	for (int i = 0; i < messages.size(); i++) {
		Dictionary message = messages[i];
		const String role = message.get("role", "");
		if (role != "user" && role != "assistant") {
			continue;
		}

		Dictionary entry;
		entry["role"] = role;
		entry["content"] = message.get("text", "");
		if (role == "user") {
			entry["images"] = _build_image_payload(message.get("images", Array()));
		}
		restore.push_back(entry);
	}
	return restore;
}

Array AIAssistantDock::_build_image_payload(const Array &p_images) const {
	Array payload;
	for (int i = 0; i < p_images.size(); i++) {
		Dictionary image = p_images[i];
		const String path = image.get("path", "");
		if (path.is_empty()) {
			continue;
		}

		Dictionary item;
		item["filename"] = image.get("filename", path.get_file());
		item["mime_type"] = _mime_type_for_path(path);
		item["data_base64"] = _read_file_base64(path);
		payload.push_back(item);
	}
	return payload;
}

void AIAssistantDock::_restore_backend_session() {
	Array restore_messages = _build_restore_messages();
	if (restore_messages.is_empty()) {
		return;
	}

	Dictionary payload;
	payload["session_id"] = session_id;
	payload["messages"] = restore_messages;

	PackedStringArray headers;
	headers.push_back("Content-Type: application/json");

	http_session->request(backend_url + "/session/restore", headers, HTTPClient::METHOD_POST, JSON::stringify(payload));
}

void AIAssistantDock::_append_images_to_transcript(const Array &p_images) {
	for (int i = 0; i < p_images.size(); i++) {
		Dictionary image = p_images[i];
		const String path = image.get("path", "");
		if (path.is_empty()) {
			continue;
		}

		Ref<Image> img = Image::load_from_file(path);
		if (img.is_null() || img->is_empty()) {
			transcript->add_text(vformat("[%s]\n", String(image.get("filename", "image"))));
			continue;
		}

		Ref<ImageTexture> tex = ImageTexture::create_from_image(img);
		int width = img->get_width();
		int height = img->get_height();
		const int max_width = 240;
		if (width > max_width) {
			height = height * max_width / width;
			width = max_width;
		}
		transcript->add_image(tex, width, height);
		transcript->add_text("\n");
	}
}

void AIAssistantDock::_append_user(const String &p_msg, const Array &p_images) {
	transcript->push_color(Color(0.6, 0.8, 1.0));
	transcript->add_text("\nYou: ");
	transcript->pop();
	if (!p_msg.is_empty()) {
		transcript->add_text(p_msg + "\n");
	}
	if (!p_images.is_empty()) {
		_append_images_to_transcript(p_images);
	}
}

String AIAssistantDock::_escape_bbcode(const String &p_text) const {
	return p_text.replace("[", "[lb]").replace("]", "[rb]");
}

String AIAssistantDock::_format_inline_markdown(const String &p_text) const {
	String out;
	int i = 0;
	const int len = p_text.length();

	while (i < len) {
		if (i + 1 < len && p_text[i] == '*' && p_text[i + 1] == '*') {
			int end = p_text.find("**", i + 2);
			if (end != -1) {
				out += "[b]" + _format_inline_markdown(p_text.substr(i + 2, end - i - 2)) + "[/b]";
				i = end + 2;
				continue;
			}
		}
		if (i + 1 < len && p_text[i] == '_' && p_text[i + 1] == '_') {
			int end = p_text.find("__", i + 2);
			if (end != -1) {
				out += "[b]" + _format_inline_markdown(p_text.substr(i + 2, end - i - 2)) + "[/b]";
				i = end + 2;
				continue;
			}
		}
		if (i + 1 < len && p_text[i] == '~' && p_text[i + 1] == '~') {
			int end = p_text.find("~~", i + 2);
			if (end != -1) {
				out += "[s]" + _format_inline_markdown(p_text.substr(i + 2, end - i - 2)) + "[/s]";
				i = end + 2;
				continue;
			}
		}
		if (p_text[i] == '`') {
			int end = p_text.find("`", i + 1);
			if (end != -1) {
				out += "[code]" + _escape_bbcode(p_text.substr(i + 1, end - i - 1)) + "[/code]";
				i = end + 1;
				continue;
			}
		}
		if (p_text[i] == '*' && (i + 1 >= len || p_text[i + 1] != '*')) {
			int end = p_text.find("*", i + 1);
			if (end != -1 && (end + 1 >= len || p_text[end + 1] != '*')) {
				out += "[i]" + _format_inline_markdown(p_text.substr(i + 1, end - i - 1)) + "[/i]";
				i = end + 1;
				continue;
			}
		}
		if (p_text[i] == '_' && (i + 1 >= len || p_text[i + 1] != '_')) {
			int end = p_text.find("_", i + 1);
			if (end != -1 && (end + 1 >= len || p_text[end + 1] != '_')) {
				out += "[i]" + _format_inline_markdown(p_text.substr(i + 1, end - i - 1)) + "[/i]";
				i = end + 1;
				continue;
			}
		}
		if (p_text[i] == '[') {
			int close_bracket = p_text.find("]", i + 1);
			int open_paren = close_bracket != -1 ? p_text.find("(", close_bracket + 1) : -1;
			int close_paren = open_paren != -1 ? p_text.find(")", open_paren + 1) : -1;
			if (close_bracket != -1 && open_paren == close_bracket + 1 && close_paren != -1) {
				String label = p_text.substr(i + 1, close_bracket - i - 1);
				String url = p_text.substr(open_paren + 1, close_paren - open_paren - 1);
				out += "[url=" + url + "]" + _format_inline_markdown(label) + "[/url]";
				i = close_paren + 1;
				continue;
			}
		}
		if (p_text[i] == '<') {
			int end = p_text.find(">", i + 1);
			if (end != -1) {
				String tag = p_text.substr(i, end - i + 1);
				if (tag.ends_with("/>") || tag.begins_with("</") || tag.contains(" ")) {
					i = end + 1;
					continue;
				}
			}
		}
		if (p_text[i] == '[') {
			out += "[lb]";
		} else if (p_text[i] == ']') {
			out += "[rb]";
		} else {
			out += p_text[i];
		}
		i++;
	}

	return out;
}

String AIAssistantDock::_preprocess_mdx(const String &p_text) const {
	String result;
	PackedStringArray lines = p_text.split("\n");

	for (int line_idx = 0; line_idx < lines.size(); line_idx++) {
		String line = lines[line_idx];
		String trimmed = line.strip_edges();

		if (trimmed.begins_with("import ") || trimmed.begins_with("export ")) {
			continue;
		}

		int pos = 0;
		while (pos < line.length()) {
			int tag_start = line.find("<", pos);
			if (tag_start == -1) {
				result += line.substr(pos);
				break;
			}

			result += line.substr(pos, tag_start - pos);

			int tag_end = line.find(">", tag_start + 1);
			if (tag_end == -1) {
				result += line.substr(tag_start);
				break;
			}

			String tag = line.substr(tag_start, tag_end - tag_start + 1);
			if (tag.ends_with("/>") || tag.begins_with("<!--") || tag.begins_with("<!")) {
				pos = tag_end + 1;
				continue;
			}

			if (tag.begins_with("</")) {
				pos = tag_end + 1;
				continue;
			}

			int name_end = 1;
			while (name_end < tag.length() && (is_ascii_alphanumeric_char(tag[name_end]) || tag[name_end] == '.')) {
				name_end++;
			}
			String tag_name = tag.substr(1, name_end - 1);
			if (tag_name.is_empty()) {
				result += tag;
				pos = tag_end + 1;
				continue;
			}

			String close_tag = "</" + tag_name + ">";
			int close_pos = line.find(close_tag, tag_end + 1);
			if (close_pos != -1) {
				result += line.substr(tag_end + 1, close_pos - tag_end - 1);
				pos = close_pos + close_tag.length();
			} else {
				pos = tag_end + 1;
			}
		}

		if (line_idx < lines.size() - 1) {
			result += "\n";
		}
	}

	int comment_start = result.find("{/*");
	while (comment_start != -1) {
		int comment_end = result.find("*/}", comment_start + 3);
		if (comment_end == -1) {
			break;
		}
		result = result.substr(0, comment_start) + result.substr(comment_end + 3);
		comment_start = result.find("{/*");
	}

	return result.replace("<br/>", "\n").replace("<br />", "\n").replace("<br>", "\n");
}

static bool _is_table_separator_row(const String &p_line) {
	PackedStringArray cells = p_line.split("|", false);
	if (cells.is_empty()) {
		return false;
	}

	for (int i = 0; i < cells.size(); i++) {
		String cell = cells[i].strip_edges();
		if (cell.is_empty()) {
			continue;
		}
		for (int j = 0; j < cell.length(); j++) {
			char32_t c = cell[j];
			if (c != '-' && c != ':' && c != ' ') {
				return false;
			}
		}
	}
	return true;
}

static PackedStringArray _split_table_cells(const String &p_line) {
	PackedStringArray raw = p_line.split("|", false);
	PackedStringArray cells;
	for (int i = 0; i < raw.size(); i++) {
		String cell = raw[i].strip_edges();
		if (!cell.is_empty() || raw.size() > 2) {
			cells.push_back(cell);
		}
	}
	return cells;
}

String AIAssistantDock::_markdown_to_bbcode(const String &p_markdown) const {
	const String source = _preprocess_mdx(p_markdown.replace("\r\n", "\n"));
	PackedStringArray lines = source.split("\n");

	String out;
	bool in_code_fence = false;
	String code_buffer;
	bool in_ul = false;
	bool in_ol = false;
	bool in_table = false;
	int table_columns = 0;

	auto close_lists = [&]() {
		if (in_ul) {
			out += "[/ul]\n";
			in_ul = false;
		}
		if (in_ol) {
			out += "[/ol]\n";
			in_ol = false;
		}
	};

	auto close_table = [&]() {
		if (in_table) {
			out += "[/table]\n\n";
			in_table = false;
			table_columns = 0;
		}
	};

	for (int line_idx = 0; line_idx < lines.size(); line_idx++) {
		String line = lines[line_idx];
		String stripped = line.strip_edges();

		if (in_code_fence) {
			if (stripped.begins_with("```")) {
				out += "[code]" + _escape_bbcode(code_buffer.strip_edges()) + "[/code]\n\n";
				code_buffer = "";
				in_code_fence = false;
			} else {
				code_buffer += line;
				if (line_idx < lines.size() - 1) {
					code_buffer += "\n";
				}
			}
			continue;
		}

		if (stripped.begins_with("```")) {
			close_lists();
			close_table();
			in_code_fence = true;
			code_buffer = "";
			continue;
		}

		if (stripped.begins_with("|") && stripped.contains("|")) {
			if (_is_table_separator_row(stripped)) {
				continue;
			}

			PackedStringArray cells = _split_table_cells(stripped);
			if (cells.is_empty()) {
				close_table();
				continue;
			}

			close_lists();
			if (!in_table) {
				table_columns = cells.size();
				out += "[table=" + itos(table_columns) + "]\n";
				in_table = true;
			}

			for (int i = 0; i < cells.size(); i++) {
				out += "[cell]" + _format_inline_markdown(cells[i]) + "[/cell]";
			}
			out += "\n";
			continue;
		}

		close_table();

		if (stripped.begins_with("#")) {
			int level = 0;
			while (level < stripped.length() && stripped[level] == '#') {
				level++;
			}
			if (level > 0 && level < stripped.length() && stripped[level] == ' ') {
				close_lists();
				const int font_size = MAX(14, 26 - level * 2);
				String title = stripped.substr(level + 1).strip_edges();
				out += vformat("[font_size=%d][b]%s[/b][/font_size]\n\n", font_size, _format_inline_markdown(title));
				continue;
			}
		}

		if (stripped.begins_with("- ") || stripped.begins_with("* ")) {
			if (in_ol) {
				out += "[/ol]\n";
				in_ol = false;
			}
			if (!in_ul) {
				out += "[ul]\n";
				in_ul = true;
			}
			out += _format_inline_markdown(stripped.substr(2).strip_edges()) + "\n";
			continue;
		}

		if (stripped.length() > 2) {
			int dot_pos = 0;
			while (dot_pos < stripped.length() && is_digit(stripped[dot_pos])) {
				dot_pos++;
			}
			if (dot_pos > 0 && dot_pos + 1 < stripped.length() && stripped[dot_pos] == '.' && stripped[dot_pos + 1] == ' ') {
				if (in_ul) {
					out += "[/ul]\n";
					in_ul = false;
				}
				if (!in_ol) {
					out += "[ol]\n";
					in_ol = true;
				}
				out += _format_inline_markdown(stripped.substr(dot_pos + 2).strip_edges()) + "\n";
				continue;
			}
		}

		if (stripped.begins_with(">")) {
			close_lists();
			String quote = stripped.substr(1).strip_edges();
			out += "[i]" + _format_inline_markdown(quote) + "[/i]\n\n";
			continue;
		}

		if (stripped == "---" || stripped == "***" || stripped == "___") {
			close_lists();
			out += "\n";
			continue;
		}

		if (stripped.is_empty()) {
			close_lists();
			out += "\n";
			continue;
		}

		close_lists();
		out += _format_inline_markdown(stripped) + "\n\n";
	}

	if (in_code_fence && !code_buffer.is_empty()) {
		out += "[code]" + _escape_bbcode(code_buffer.strip_edges()) + "[/code]\n\n";
	}
	close_lists();
	close_table();

	return out;
}

void AIAssistantDock::_append_assistant(const String &p_msg) {
	transcript->push_color(Color(0.85, 0.95, 0.7));
	transcript->add_text("\nAgent: ");
	transcript->pop();
	transcript->append_text(_markdown_to_bbcode(p_msg));
	transcript->add_text("\n");
}

void AIAssistantDock::_append_system(const String &p_msg) {
	transcript->push_color(Color(0.7, 0.7, 0.7));
	transcript->add_text("\n[" + p_msg + "]\n");
	transcript->pop();
}

Dictionary AIAssistantDock::_gather_project_context() const {
	Dictionary ctx;
	ctx["project_path"] = ProjectSettings::get_singleton()->get_resource_path();
	Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
	if (edited_scene) {
		ctx["current_scene"] = edited_scene->get_scene_file_path();
	} else {
		ctx["current_scene"] = String();
	}
	ctx["godot_version"] = String(GODOT_VERSION_FULL_NAME);
	return ctx;
}

void AIAssistantDock::_continue_chat() {
	Dictionary payload;
	payload["session_id"] = session_id;
	payload["prompt"] = String();
	payload["context"] = _gather_project_context();
	payload["images"] = Array();

	PackedStringArray headers;
	headers.push_back("Content-Type: application/json");

	Error err = http_chat->request(backend_url + "/chat", headers, HTTPClient::METHOD_POST, JSON::stringify(payload));
	if (err != OK) {
		_append_system(vformat("Continue request failed: %d", (int)err));
		_set_busy(false);
	}
}

void AIAssistantDock::_on_send_pressed() {
	if (busy) {
		return;
	}

	String prompt = input->get_text().strip_edges();
	if (prompt.is_empty() && pending_images.is_empty()) {
		return;
	}

	Array images_to_send = pending_images.duplicate();
	input->set_text("");
	pending_images.clear();
	_refresh_attachment_preview();
	_append_user(prompt, images_to_send);
	_record_message("user", prompt, images_to_send);
	_set_busy(true);
	status_line->set_text(TTR("Thinking…"));

	Dictionary payload;
	payload["session_id"] = session_id;
	payload["prompt"] = prompt;
	payload["context"] = _gather_project_context();
	payload["images"] = _build_image_payload(images_to_send);

	PackedStringArray headers;
	headers.push_back("Content-Type: application/json");

	Error err = http_chat->request(backend_url + "/chat", headers, HTTPClient::METHOD_POST, JSON::stringify(payload));
	if (err != OK) {
		_append_system(vformat("Request failed: %d", (int)err));
		_record_message("system", vformat("Request failed: %d", (int)err));
		_set_busy(false);
	}
}

void AIAssistantDock::_on_stop_pressed() {
	http_chat->cancel_request();
	_append_system("Cancelled");
	_record_message("system", "Cancelled");
	pending_tool_results = 0;
	_set_busy(false);
}

void AIAssistantDock::_on_chat_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	if (p_result != HTTPRequest::RESULT_SUCCESS || p_response_code != 200) {
		const String err_msg = vformat("HTTP error result=%d code=%d", p_result, p_response_code);
		_append_system(err_msg);
		_record_message("system", err_msg);
		_set_busy(false);
		return;
	}

	String response_text;
	response_text.append_utf8((const char *)p_body.ptr(), p_body.size());
	Variant parsed = JSON::parse_string(response_text);
	if (parsed.get_type() != Variant::DICTIONARY) {
		_append_system("Bad response (not JSON object).");
		_set_busy(false);
		return;
	}

	Dictionary response = parsed;
	if (response.has("text")) {
		String text = response["text"];
		if (!text.is_empty()) {
			_append_assistant(text);
			_record_message("assistant", text);
		}
	}

	if (response.has("tool_calls")) {
		Array tool_calls = response["tool_calls"];
		if (!tool_calls.is_empty()) {
			_dispatch_tool_calls(tool_calls);
			return;
		}
	}

	_set_busy(false);
}

void AIAssistantDock::_on_health_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	if (p_result == HTTPRequest::RESULT_SUCCESS && p_response_code == 200) {
		status_line->set_text(TTR("Backend connected"));
	} else {
		status_line->set_text(TTR("Backend offline — start: ai-backend"));
	}
}

void AIAssistantDock::_on_tool_result_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	pending_tool_results--;
	if (pending_tool_results <= 0) {
		pending_tool_results = 0;
		_continue_chat();
	}
}

void AIAssistantDock::_post_tool_result(const String &p_tool_call_id, const String &p_result) {
	Dictionary payload;
	payload["session_id"] = session_id;
	payload["tool_call_id"] = p_tool_call_id;
	payload["result"] = JSON::parse_string(p_result);

	PackedStringArray headers;
	headers.push_back("Content-Type: application/json");

	Error err = http_tool->request(backend_url + "/tool_result", headers, HTTPClient::METHOD_POST, JSON::stringify(payload));
	if (err != OK) {
		pending_tool_results--;
		if (pending_tool_results <= 0) {
			_continue_chat();
		}
	}
}

void AIAssistantDock::_dispatch_tool_calls(const Array &p_calls) {
	pending_tool_results = p_calls.size();
	for (int i = 0; i < p_calls.size(); i++) {
		Dictionary call = p_calls[i];
		String name = call.get("name", "");
		String result = _execute_tool(call);
		const String note = vformat("tool: %s -> %s", name, result.left(120));
		_append_system(note);
		_record_message("system", note);
		_post_tool_result(call.get("id", ""), result);
	}
}

String AIAssistantDock::_execute_tool(const Dictionary &p_call) {
	String name = p_call.get("name", "");
	Dictionary args = p_call.get("args", Dictionary());

	if (name == "read_file") {
		return _tool_read_file(args.get("path", ""));
	}
	if (name == "write_file") {
		return _tool_write_file(args.get("path", ""), args.get("contents", ""));
	}
	if (name == "write_binary_file") {
		return _tool_write_binary_file(args.get("path", ""), args.get("data_base64", ""));
	}
	if (name == "delete_file") {
		return _tool_delete_file(args.get("path", ""));
	}
	if (name == "make_dir") {
		return _tool_make_dir(args.get("path", ""));
	}
	if (name == "list_dir") {
		return _tool_list_dir(args.get("path", "res://"));
	}
	if (name == "download_url") {
		return _tool_download_url(args.get("url", ""), args.get("path", ""));
	}
	if (name == "extract_zip") {
		return _tool_extract_zip(args.get("zip_path", ""), args.get("target_dir", ""));
	}
	if (name == "open_scene") {
		return _tool_open_scene(args.get("path", ""));
	}
	if (name == "save_scene") {
		return _tool_save_scene(args.get("path", ""));
	}
	if (name == "get_scene_tree") {
		return _tool_get_scene_tree();
	}
	if (name == "get_open_script_text") {
		return _tool_get_open_script_text();
	}
	if (name == "get_project_info") {
		return _tool_get_project_info();
	}
	if (name == "set_project_setting") {
		return _tool_set_project_setting(args.get("key", ""), args.get("value", ""));
	}
	if (name == "set_main_scene") {
		return _tool_set_main_scene(args.get("path", ""));
	}
	if (name == "set_autoload") {
		return _tool_set_autoload(args.get("name", ""), args.get("path", ""), args.get("singleton", true));
	}
	if (name == "scan_filesystem") {
		return _tool_scan_filesystem();
	}
	if (name == "create_svg") {
		return _tool_create_svg(args.get("path", ""), args.get("contents", ""), args.get("width", 0), args.get("height", 0));
	}
	if (name == "run_project") {
		return _tool_run_project(args.get("scene_path", ""), args.get("quit_after_frames", 60));
	}
	if (name == "verify_deliverables") {
		return _tool_verify_deliverables(args.get("paths", Array()), args.get("require_main_scene", false));
	}

	Dictionary out;
	out["ok"] = false;
	out["error"] = "unknown_tool";
	return JSON::stringify(out);
}

PackedByteArray AIAssistantDock::_http_download_sync(const String &p_url) const {
	PackedByteArray body;
	if (p_url.is_empty()) {
		return body;
	}

	Ref<HTTPClient> client = HTTPClient::create();
	String scheme = "http://";
	String host = p_url;
	int port = 80;
	String path = "/";

	if (p_url.begins_with("https://")) {
		scheme = "https://";
		port = 443;
		host = p_url.substr(8);
	} else if (p_url.begins_with("http://")) {
		host = p_url.substr(7);
	}

	int slash_pos = host.find_char('/');
	if (slash_pos != -1) {
		path = host.substr(slash_pos);
		host = host.substr(0, slash_pos);
	}

	int colon_pos = host.find_char(':');
	if (colon_pos != -1) {
		port = host.substr(colon_pos + 1).to_int();
		host = host.substr(0, colon_pos);
	}

	Error err = client->connect_to_host(host, port, scheme == "https://" ? TLSOptions::client() : Ref<TLSOptions>());
	if (err != OK) {
		return body;
	}

	const int timeout_ms = 120000;
	const uint64_t start = OS::get_singleton()->get_ticks_msec();
	while (client->get_status() == HTTPClient::STATUS_CONNECTING || client->get_status() == HTTPClient::STATUS_RESOLVING) {
		client->poll();
		OS::get_singleton()->delay_usec(10000);
		if (OS::get_singleton()->get_ticks_msec() - start > timeout_ms) {
			return body;
		}
	}

	if (client->get_status() != HTTPClient::STATUS_CONNECTED) {
		return body;
	}

	Vector<String> request_headers;
	err = client->request(HTTPClient::METHOD_GET, path, request_headers, nullptr, 0);
	if (err != OK) {
		return body;
	}

	while (client->get_status() == HTTPClient::STATUS_REQUESTING) {
		client->poll();
		OS::get_singleton()->delay_usec(10000);
		if (OS::get_singleton()->get_ticks_msec() - start > timeout_ms) {
			return body;
		}
	}

	if (client->get_status() < HTTPClient::STATUS_BODY || client->get_response_code() != 200) {
		return body;
	}

	while (client->get_status() == HTTPClient::STATUS_BODY) {
		client->poll();
		body.append_array(client->read_response_body_chunk());
		OS::get_singleton()->delay_usec(1000);
		if (OS::get_singleton()->get_ticks_msec() - start > timeout_ms) {
			break;
		}
	}

	return body;
}

Dictionary AIAssistantDock::_node_to_dict(Node *p_node) const {
	Dictionary node_data;
	if (!p_node) {
		return node_data;
	}

	node_data["name"] = p_node->get_name();
	node_data["type"] = p_node->get_class();
	node_data["path"] = String(p_node->get_path());
	Ref<Script> script = p_node->get_script();
	if (script.is_valid()) {
		node_data["script"] = script->get_path();
	}

	Array children;
	for (int i = 0; i < p_node->get_child_count(); i++) {
		children.push_back(_node_to_dict(p_node->get_child(i)));
	}
	node_data["children"] = children;
	return node_data;
}

String AIAssistantDock::_tool_read_file(const String &p_path) {
	Error err = OK;
	String contents = FileAccess::get_file_as_string(p_path, &err);
	Dictionary out;
	out["ok"] = (err == OK);
	out["contents"] = contents;
	if (err != OK) {
		out["error"] = vformat("read failed (%d)", (int)err);
	}
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_write_file(const String &p_path, const String &p_contents) {
	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &err);
	Dictionary out;
	if (err != OK || file.is_null()) {
		out["ok"] = false;
		out["error"] = vformat("write failed (%d)", (int)err);
		return JSON::stringify(out);
	}
	file->store_string(p_contents);
	file->close();
	EditorNode::get_log()->add_message(vformat("AI agent wrote: %s", p_path), EditorLog::MSG_TYPE_EDITOR);
	out["ok"] = true;
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_write_binary_file(const String &p_path, const String &p_data_base64) {
	Dictionary out;
	const Vector<uint8_t> data = CoreBind::Marshalls::get_singleton()->base64_to_raw(p_data_base64);
	if (data.is_empty() && !p_data_base64.is_empty()) {
		out["ok"] = false;
		out["error"] = "invalid base64";
		return JSON::stringify(out);
	}

	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &err);
	if (err != OK || file.is_null()) {
		out["ok"] = false;
		out["error"] = vformat("write failed (%d)", (int)err);
		return JSON::stringify(out);
	}
	file->store_buffer(data);
	file->close();
	EditorNode::get_log()->add_message(vformat("AI agent wrote binary: %s", p_path), EditorLog::MSG_TYPE_EDITOR);
	out["ok"] = true;
	out["bytes"] = (int)data.size();
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_delete_file(const String &p_path) {
	Dictionary out;
	Error err = DirAccess::remove_absolute(ProjectSettings::get_singleton()->globalize_path(p_path));
	out["ok"] = (err == OK);
	if (err != OK) {
		out["error"] = vformat("delete failed (%d)", (int)err);
	}
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_make_dir(const String &p_path) {
	Dictionary out;
	Ref<DirAccess> dir = DirAccess::open("res://");
	if (dir.is_null()) {
		out["ok"] = false;
		out["error"] = "no res:// access";
		return JSON::stringify(out);
	}

	String local_path = p_path;
	if (local_path.begins_with("res://")) {
		local_path = local_path.substr(6);
	}
	Error err = dir->make_dir_recursive(local_path);
	out["ok"] = (err == OK || err == ERR_ALREADY_EXISTS);
	if (err != OK && err != ERR_ALREADY_EXISTS) {
		out["error"] = vformat("mkdir failed (%d)", (int)err);
	}
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_download_url(const String &p_url, const String &p_path) {
	Dictionary out;
	if (p_url.is_empty() || p_path.is_empty()) {
		out["ok"] = false;
		out["error"] = "url and path required";
		return JSON::stringify(out);
	}

	const PackedByteArray data = _http_download_sync(p_url);
	if (data.is_empty()) {
		out["ok"] = false;
		out["error"] = "download failed or empty response";
		return JSON::stringify(out);
	}

	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &err);
	if (err != OK || file.is_null()) {
		out["ok"] = false;
		out["error"] = vformat("write failed (%d)", (int)err);
		return JSON::stringify(out);
	}
	file->store_buffer(data);
	file->close();
	EditorNode::get_log()->add_message(vformat("AI agent downloaded: %s", p_path), EditorLog::MSG_TYPE_EDITOR);
	out["ok"] = true;
	out["path"] = p_path;
	out["bytes"] = (int)data.size();
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_extract_zip(const String &p_zip_path, const String &p_target_dir) {
	Dictionary out;
	if (p_zip_path.is_empty() || p_target_dir.is_empty()) {
		out["ok"] = false;
		out["error"] = "zip_path and target_dir required";
		return JSON::stringify(out);
	}

#ifndef MODULE_ZIP_ENABLED
	out["ok"] = false;
	out["error"] = "zip module not enabled in this build";
	return JSON::stringify(out);
#else
	Ref<ZIPReader> zip;
	zip.instantiate();
	Error err = zip->open(p_zip_path);
	if (err != OK) {
		out["ok"] = false;
		out["error"] = vformat("zip open failed (%d)", (int)err);
		return JSON::stringify(out);
	}

	Ref<DirAccess> dir = DirAccess::open("res://");
	if (dir.is_null()) {
		zip->close();
		out["ok"] = false;
		out["error"] = "no res:// access";
		return JSON::stringify(out);
	}

	String target_local = p_target_dir;
	if (target_local.begins_with("res://")) {
		target_local = target_local.substr(6);
	}
	dir->make_dir_recursive(target_local);

	int extracted = 0;
	const PackedStringArray files = zip->get_files();
	for (int i = 0; i < files.size(); i++) {
		const String entry = files[i];
		if (entry.is_empty()) {
			continue;
		}

		const PackedByteArray file_data = zip->read_file(entry);
		String output_path = p_target_dir.path_join(entry);
		if (output_path.begins_with("res://")) {
			String output_local = output_path.substr(6);
			const int last_slash = output_local.rfind_char('/');
			if (last_slash != -1) {
				dir->make_dir_recursive(output_local.substr(0, last_slash));
			}
		}

		Ref<FileAccess> file = FileAccess::open(output_path, FileAccess::WRITE);
		if (file.is_null()) {
			continue;
		}
		file->store_buffer(file_data);
		file->close();
		extracted++;
	}

	zip->close();
	out["ok"] = true;
	out["extracted_files"] = extracted;
	out["target_dir"] = p_target_dir;
	return JSON::stringify(out);
#endif
}

String AIAssistantDock::_tool_save_scene(const String &p_path) {
	Dictionary out;
	Node *scene = EditorNode::get_singleton()->get_edited_scene();
	if (!scene) {
		out["ok"] = false;
		out["error"] = "no edited scene";
		return JSON::stringify(out);
	}

	String save_path = p_path;
	if (save_path.is_empty()) {
		save_path = scene->get_scene_file_path();
	}
	if (save_path.is_empty()) {
		out["ok"] = false;
		out["error"] = "scene has no path; provide path";
		return JSON::stringify(out);
	}

	EditorNode::get_singleton()->save_scene_to_path(save_path, false);
	out["ok"] = true;
	out["path"] = save_path;
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_get_scene_tree() {
	Dictionary out;
	Node *scene = EditorNode::get_singleton()->get_edited_scene();
	if (!scene) {
		out["ok"] = false;
		out["error"] = "no edited scene";
		return JSON::stringify(out);
	}

	out["ok"] = true;
	out["scene_path"] = scene->get_scene_file_path();
	out["root"] = _node_to_dict(scene);
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_get_project_info() {
	Dictionary out;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	out["ok"] = true;
	out["project_path"] = settings->get_resource_path();
	out["project_name"] = settings->get_setting("application/config/name");
	out["main_scene"] = settings->get_setting("application/run/main_scene");
	out["features"] = settings->get_setting("application/config/features");
	out["godot_version"] = String(GODOT_VERSION_FULL_NAME);

	Array autoloads;
	const HashMap<StringName, ProjectSettings::AutoloadInfo> &autoload_map = settings->get_autoload_list();
	for (const KeyValue<StringName, ProjectSettings::AutoloadInfo> &E : autoload_map) {
		Dictionary entry;
		entry["name"] = E.key;
		entry["path"] = E.value.path;
		entry["singleton"] = E.value.is_singleton;
		autoloads.push_back(entry);
	}
	out["autoloads"] = autoloads;

	Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
	if (edited_scene) {
		out["edited_scene"] = edited_scene->get_scene_file_path();
	} else {
		out["edited_scene"] = String();
	}

	Array input_actions;
	if (InputMap::get_singleton()) {
		const TypedArray<StringName> actions = InputMap::get_singleton()->get_actions();
		for (int i = 0; i < actions.size(); i++) {
			input_actions.push_back(String(actions[i]));
		}
	}
	out["input_actions"] = input_actions;
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_set_project_setting(const String &p_key, const String &p_value_json) {
	Dictionary out;
	if (p_key.is_empty()) {
		out["ok"] = false;
		out["error"] = "key required";
		return JSON::stringify(out);
	}

	Variant value = JSON::parse_string(p_value_json);
	if (value.get_type() == Variant::NIL && p_value_json != "null") {
		value = p_value_json;
	}

	ProjectSettings::get_singleton()->set_setting(p_key, value);
	ProjectSettings::get_singleton()->save();
	out["ok"] = true;
	out["key"] = p_key;
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_set_main_scene(const String &p_path) {
	Dictionary out;
	if (p_path.is_empty()) {
		out["ok"] = false;
		out["error"] = "path required";
		return JSON::stringify(out);
	}

	ProjectSettings::get_singleton()->set_setting("application/run/main_scene", p_path);
	ProjectSettings::get_singleton()->save();
	out["ok"] = true;
	out["main_scene"] = p_path;
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_set_autoload(const String &p_name, const String &p_path, bool p_singleton) {
	Dictionary out;
	if (p_name.is_empty() || p_path.is_empty()) {
		out["ok"] = false;
		out["error"] = "name and path required";
		return JSON::stringify(out);
	}

	const String key = "autoload/" + p_name;
	const String value = p_singleton ? "*" + p_path : p_path;
	ProjectSettings::get_singleton()->set_setting(key, value);
	ProjectSettings::get_singleton()->save();
	out["ok"] = true;
	out["name"] = p_name;
	out["path"] = p_path;
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_scan_filesystem() {
	Dictionary out;
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->scan();
		out["ok"] = true;
	} else {
		out["ok"] = false;
		out["error"] = "filesystem not ready";
	}
	return JSON::stringify(out);
}

Array AIAssistantDock::_extract_run_errors(const String &p_output) const {
	Array errors;
	const PackedStringArray lines = p_output.split("\n");
	for (int i = 0; i < lines.size(); i++) {
		const String line = lines[i].strip_edges();
		if (line.is_empty()) {
			continue;
		}
		if (line.contains("ERROR:") || line.contains("SCRIPT ERROR") || line.contains("Parse Error") ||
				line.contains("Failed to load") || line.contains("Invalid call") || line.contains("Cannot get class") ||
				line.contains("Parser Error") || line.contains("Compile Error")) {
			errors.push_back(line);
		}
	}
	return errors;
}

String AIAssistantDock::_tool_create_svg(const String &p_path, const String &p_contents, int p_width, int p_height) {
	Dictionary out;
	String path = p_path;
	if (!path.ends_with(".svg")) {
		path += ".svg";
	}

	String svg = p_contents.strip_edges();
	if (svg.is_empty()) {
		out["ok"] = false;
		out["error"] = "empty SVG contents";
		return JSON::stringify(out);
	}
	if (!svg.contains("<svg")) {
		out["ok"] = false;
		out["error"] = "contents must include an <svg> root element";
		return JSON::stringify(out);
	}
	if (!svg.contains("xmlns=")) {
		svg = svg.replace("<svg", "<svg xmlns=\"http://www.w3.org/2000/svg\"");
	}
	if (p_width > 0 && !svg.contains("width=")) {
		svg = svg.replace("<svg", vformat("<svg width=\"%d\"", p_width));
	}
	if (p_height > 0 && !svg.contains("height=")) {
		svg = svg.replace("<svg", vformat("<svg height=\"%d\"", p_height));
	}
	if (!svg.ends_with("\n")) {
		svg += "\n";
	}

	const String write_result = _tool_write_file(path, svg);
	Variant parsed = JSON::parse_string(write_result);
	if (parsed.get_type() != Variant::DICTIONARY || !parsed.operator Dictionary().get("ok", false)) {
		return write_result;
	}

	(void)_tool_scan_filesystem();
	out["ok"] = true;
	out["path"] = path;
	out["bytes"] = svg.length();
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_run_project(const String &p_scene_path, int p_quit_after_frames) {
	Dictionary out;
	EditorNode::get_singleton()->try_autosave();

	const String project_path = ProjectSettings::get_singleton()->get_resource_path();
	if (project_path.is_empty()) {
		out["ok"] = false;
		out["error"] = "no project open";
		return JSON::stringify(out);
	}

	String scene_path = p_scene_path;
	if (scene_path.is_empty()) {
		scene_path = GLOBAL_GET("application/run/main_scene");
	}

	const int frames = MAX(p_quit_after_frames, 1);
	const String exec = OS::get_singleton()->get_executable_path();
	List<String> args;
	args.push_back("--path");
	args.push_back(project_path);
	args.push_back("--headless");
	args.push_back("--quit-after");
	args.push_back(itos(frames));
	if (!scene_path.is_empty()) {
		args.push_back("--scene");
		args.push_back(ResourceUID::ensure_path(scene_path));
	}

	status_line->set_text(TTR("Running project (headless)…"));
	String output;
	int exit_code = 255;
	const Error exec_err = OS::get_singleton()->execute(exec, args, &output, &exit_code, true);
	status_line->set_text(TTR("Idle"));

	const Array errors = _extract_run_errors(output);
	const bool run_ok = exec_err == OK && exit_code == 0 && errors.is_empty();

	out["ok"] = run_ok;
	out["ran"] = true;
	out["exit_code"] = exit_code;
	out["exec_error"] = (int)exec_err;
	out["scene"] = scene_path;
	out["main_scene"] = GLOBAL_GET("application/run/main_scene");
	out["errors"] = errors;
	if (output.length() > 12000) {
		out["output"] = output.substr(output.length() - 12000);
		out["output_truncated"] = true;
	} else {
		out["output"] = output;
		out["output_truncated"] = false;
	}
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_verify_deliverables(const Array &p_paths, bool p_require_main_scene) {
	Dictionary out;
	Array checks;
	bool all_ok = true;

	for (int i = 0; i < p_paths.size(); i++) {
		const String path = p_paths[i];
		bool exists = false;
		if (path.ends_with("/")) {
			exists = DirAccess::dir_exists_absolute(ProjectSettings::get_singleton()->globalize_path(path));
		} else {
			exists = FileAccess::exists(path) || DirAccess::dir_exists_absolute(ProjectSettings::get_singleton()->globalize_path(path));
		}

		Dictionary check;
		check["path"] = path;
		check["exists"] = exists;
		checks.push_back(check);
		if (!exists) {
			all_ok = false;
		}
	}

	const String main_scene = GLOBAL_GET("application/run/main_scene");
	const bool has_main_scene = !String(main_scene).is_empty();
	if (p_require_main_scene && !has_main_scene) {
		all_ok = false;
	}

	out["ok"] = all_ok;
	out["checks"] = checks;
	out["require_main_scene"] = p_require_main_scene;
	out["main_scene"] = main_scene;
	out["has_main_scene"] = has_main_scene;
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_list_dir(const String &p_path) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	Dictionary out;
	if (dir.is_null()) {
		out["ok"] = false;
		out["error"] = "list_dir failed";
		return JSON::stringify(out);
	}

	Array files;
	dir->list_dir_begin();
	String entry = dir->get_next();
	while (!entry.is_empty()) {
		if (entry != "." && entry != "..") {
			Dictionary item;
			item["name"] = entry;
			item["is_dir"] = dir->current_is_dir();
			files.push_back(item);
		}
		entry = dir->get_next();
	}
	dir->list_dir_end();

	out["ok"] = true;
	out["entries"] = files;
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_open_scene(const String &p_path) {
	Error err = EditorNode::get_singleton()->load_scene(p_path);
	Dictionary out;
	out["ok"] = (err == OK);
	if (err != OK) {
		out["error"] = vformat("load_scene failed (%d)", (int)err);
	}
	return JSON::stringify(out);
}

String AIAssistantDock::_tool_get_open_script_text() {
	Dictionary out;
	Ref<Script> script;

	Node *edited_scene = EditorNode::get_singleton()->get_edited_scene();
	if (edited_scene) {
		script = edited_scene->get_script();
	}

	if (!script.is_valid() && ScriptEditor::get_singleton()) {
		Vector<Ref<Script>> open_scripts = ScriptEditor::get_singleton()->get_open_scripts();
		if (!open_scripts.is_empty()) {
			script = open_scripts[0];
		}
	}

	if (!script.is_valid()) {
		out["ok"] = false;
		out["error"] = "no open script";
		return JSON::stringify(out);
	}

	out["ok"] = true;
	out["path"] = script->get_path();
	out["text"] = script->get_source_code();
	return JSON::stringify(out);
}

void AIAssistantDock::_on_input_text_changed() {
	_update_send_enabled();
}

void AIAssistantDock::_on_new_chat_pressed() {
	if (busy) {
		return;
	}
	_create_new_chat(true);
}

void AIAssistantDock::_on_chat_selected(int p_index) {
	if (busy || p_index < 0 || p_index >= chats.size()) {
		return;
	}

	const String chat_id = chats[p_index].operator Dictionary().get("id", "");
	if (chat_id == active_chat_id) {
		return;
	}

	_switch_to_chat(chat_id);
}

void AIAssistantDock::_on_attach_pressed() {
	if (busy) {
		return;
	}
	image_dialog->popup_file_dialog();
}

void AIAssistantDock::_on_image_selected(const String &p_path) {
	if (p_path.is_empty()) {
		return;
	}

	const String saved_path = _save_image_attachment(p_path, p_path.get_file());
	if (saved_path.is_empty()) {
		_append_system(TTR("Failed to attach image."));
		return;
	}

	Dictionary image;
	image["path"] = saved_path;
	image["filename"] = p_path.get_file();
	pending_images.push_back(image);
	_refresh_attachment_preview();
	_update_send_enabled();
}

void AIAssistantDock::_refresh_attachment_preview() {
	for (int i = attachment_preview->get_child_count() - 1; i >= 0; i--) {
		attachment_preview->get_child(i)->queue_free();
	}

	for (int i = 0; i < pending_images.size(); i++) {
		Dictionary image = pending_images[i];
		const String path = image.get("path", "");
		if (path.is_empty()) {
			continue;
		}

		Ref<Image> img = Image::load_from_file(path);
		if (img.is_null() || img->is_empty()) {
			continue;
		}

		Ref<ImageTexture> tex = ImageTexture::create_from_image(img);
		TextureRect *preview = memnew(TextureRect);
		preview->set_texture(tex);
		preview->set_custom_minimum_size(Size2(48, 48));
		preview->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
		preview->set_tooltip_text(image.get("filename", path.get_file()));
		attachment_preview->add_child(preview);
	}

	attachment_preview->set_visible(!pending_images.is_empty());
}

void AIAssistantDock::_on_session_restore_completed(int p_result, int p_response_code, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	if (p_result != HTTPRequest::RESULT_SUCCESS || p_response_code != 200) {
		_append_system(TTR("Could not restore chat context on backend."));
	}
}

void AIAssistantDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			http_health->request(backend_url + "/health");
			_load_chats();
		} break;
		case NOTIFICATION_PREDELETE: {
			_save_chats();
		} break;
	}
}

AIAssistantDock::AIAssistantDock() {
	set_name(TTRC("AI Assistant"));
	set_icon_name("Script");
	set_default_slot(EditorDock::DOCK_SLOT_LEFT_UL);
	set_dock_shortcut(ED_SHORTCUT_AND_COMMAND("docks/open_ai_assistant", TTRC("Open AI Assistant")));

	VBoxContainer *root = memnew(VBoxContainer);
	add_child(root);

	HBoxContainer *top_row = memnew(HBoxContainer);
	root->add_child(top_row);

	chat_selector = memnew(OptionButton);
	chat_selector->set_h_size_flags(SIZE_EXPAND_FILL);
	chat_selector->connect(SceneStringName(item_selected), callable_mp(this, &AIAssistantDock::_on_chat_selected));
	top_row->add_child(chat_selector);

	new_chat_button = memnew(Button);
	new_chat_button->set_text(TTR("New Chat"));
	new_chat_button->connect(SceneStringName(pressed), callable_mp(this, &AIAssistantDock::_on_new_chat_pressed));
	top_row->add_child(new_chat_button);

	status_line = memnew(LineEdit);
	status_line->set_editable(false);
	status_line->set_text(TTR("Connecting to backend…"));
	root->add_child(status_line);

	transcript = memnew(RichTextLabel);
	transcript->set_use_bbcode(true);
	transcript->set_scroll_follow(true);
	transcript->set_selection_enabled(true);
	transcript->set_v_size_flags(SIZE_EXPAND_FILL);
	root->add_child(transcript);

	attachment_preview = memnew(HBoxContainer);
	attachment_preview->set_visible(false);
	root->add_child(attachment_preview);

	input = memnew(TextEdit);
	input->set_custom_minimum_size(Size2(0, 80));
	input->set_placeholder(TTR("Ask the agent to build, debug, or explain…"));
	input->connect(SceneStringName(text_changed), callable_mp(this, &AIAssistantDock::_on_input_text_changed));
	root->add_child(input);

	HBoxContainer *btn_row = memnew(HBoxContainer);
	root->add_child(btn_row);

	attach_button = memnew(Button);
	attach_button->set_text(TTR("Attach Image"));
	attach_button->connect(SceneStringName(pressed), callable_mp(this, &AIAssistantDock::_on_attach_pressed));
	btn_row->add_child(attach_button);

	send_button = memnew(Button);
	send_button->set_text(TTR("Send"));
	send_button->set_disabled(true);
	send_button->connect(SceneStringName(pressed), callable_mp(this, &AIAssistantDock::_on_send_pressed));
	btn_row->add_child(send_button);

	stop_button = memnew(Button);
	stop_button->set_text(TTR("Stop"));
	stop_button->set_disabled(true);
	stop_button->connect(SceneStringName(pressed), callable_mp(this, &AIAssistantDock::_on_stop_pressed));
	btn_row->add_child(stop_button);

	image_dialog = memnew(EditorFileDialog);
	image_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	image_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	image_dialog->add_filter("*.png,*.jpg,*.jpeg,*.webp,*.gif,*.bmp", TTR("Images"));
	image_dialog->set_title(TTR("Attach Image"));
	add_child(image_dialog);
	image_dialog->connect("file_selected", callable_mp(this, &AIAssistantDock::_on_image_selected));

	http_health = memnew(HTTPRequest);
	http_health->set_timeout(10.0);
	add_child(http_health);
	http_health->connect("request_completed", callable_mp(this, &AIAssistantDock::_on_health_completed));

	http_chat = memnew(HTTPRequest);
	http_chat->set_timeout(120.0);
	add_child(http_chat);
	http_chat->connect("request_completed", callable_mp(this, &AIAssistantDock::_on_chat_completed));

	http_tool = memnew(HTTPRequest);
	http_tool->set_timeout(30.0);
	add_child(http_tool);
	http_tool->connect("request_completed", callable_mp(this, &AIAssistantDock::_on_tool_result_completed));

	http_session = memnew(HTTPRequest);
	http_session->set_timeout(30.0);
	add_child(http_session);
	http_session->connect("request_completed", callable_mp(this, &AIAssistantDock::_on_session_restore_completed));
}
