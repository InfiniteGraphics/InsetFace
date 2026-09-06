#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "resource.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Shell32.lib")

namespace {

const wchar_t* kAppTitle = L"InsetFace Installer";
const wchar_t* kProcessName = L"Metaseq.exe";
const wchar_t* kPluginFileName = L"InsetFace.dll";
const wchar_t* kIconFileName = L"cmd_insetface.svg";
const wchar_t* kResourceFileName = L"InsetFace.resource.xml";
const wchar_t* kManifestFileName = L"InsetFace.install.json";
const wchar_t* kRegistryUninstall = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
const wchar_t* kRegistryUninstallWow6432 = L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
const wchar_t* kRegistryAppPaths = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Metaseq.exe";
const char* kWidgetName = "BtnComPlugin.56a31d20.5a1e7f44.0";
const char* kInstallerVersion = "1.01";

enum class InstallAction
{
	Install,
	Repair,
	Uninstall,
};

enum class LayoutPatchKind
{
	Default,
	StandardIcon,
	Basic,
	Mapping,
	Unknown,
};

struct XmlWidgetTag
{
	std::vector<std::string> Order;
	std::map<std::string, std::string> Attributes;
};

struct LayoutManifestEntry
{
	std::wstring Name;
	std::wstring Path;
	std::string Action;
	std::string OriginalTag;
};

struct InstallManifest
{
	std::wstring DllPath;
	std::wstring WhiteIconPath;
	std::wstring BlackIconPath;
	std::string DllHash;
	std::string WhiteIconHash;
	std::string BlackIconHash;
	std::vector<LayoutManifestEntry> Layouts;
};

struct AppState
{
	HINSTANCE Instance;
	HWND Window;
	HWND PathEdit;
	HWND BrowseButton;
	HWND InstallButton;
	HWND RepairButton;
	HWND UninstallButton;
	HWND BrandIcon;
	HWND HeaderLabel;
	HWND HintLabel;
	HWND StatusLabel;
	HWND StatusEdit;
	HFONT UiFont;
	HFONT HeaderFont;
	HFONT HintFont;
	HFONT MonoFont;
	HICON AppIconLarge;
	HICON AppIconSmall;
	HBRUSH BackgroundBrush;
	HBRUSH CardBrush;
	HBRUSH InputBrush;
	HBRUSH AccentBrush;
};

AppState* g_app = NULL;

static std::wstring Trim(const std::wstring& value)
{
	size_t first = 0;
	while (first < value.size() && iswspace(value[first])) {
		++first;
	}
	size_t last = value.size();
	while (last > first && iswspace(value[last - 1])) {
		--last;
	}
	return value.substr(first, last - first);
}

static bool IsDirectorySeparator(wchar_t ch)
{
	return ch == L'\\' || ch == L'/';
}

static std::wstring ToLowerWide(std::wstring value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
		return static_cast<wchar_t>(towlower(ch));
	});
	return value;
}

static bool ContainsInsensitive(const std::wstring& value, const std::wstring& needle)
{
	return ToLowerWide(value).find(ToLowerWide(needle)) != std::wstring::npos;
}

static bool EndsWithInsensitive(const std::wstring& value, const std::wstring& suffix)
{
	if (value.size() < suffix.size()) return false;
	return ToLowerWide(value.substr(value.size() - suffix.size())) == ToLowerWide(suffix);
}

static std::wstring GetDirectoryName(const std::wstring& path)
{
	size_t pos = path.find_last_of(L"\\/");
	if (pos == std::wstring::npos) return std::wstring();
	return path.substr(0, pos);
}

static std::wstring JoinPath(const std::wstring& left, const std::wstring& right)
{
	if (left.empty()) return right;
	if (right.empty()) return left;
	if (IsDirectorySeparator(left[left.size() - 1])) return left + right;
	return left + L"\\" + right;
}

static std::wstring NormalizePath(std::wstring value)
{
	value = Trim(value);
	if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
		value = value.substr(1, value.size() - 2);
	}

	size_t comma = value.find(L',');
	if (comma != std::wstring::npos && EndsWithInsensitive(value.substr(0, comma), L".exe")) {
		value = value.substr(0, comma);
	}

	if (EndsWithInsensitive(value, L"\\Metaseq.exe") || EndsWithInsensitive(value, L"/Metaseq.exe")) {
		value = GetDirectoryName(value);
	}

	if (value.empty()) return value;

	DWORD length = GetFullPathNameW(value.c_str(), 0, NULL, NULL);
	if (length > 0) {
		std::vector<wchar_t> buffer(length);
		if (GetFullPathNameW(value.c_str(), length, buffer.data(), NULL) > 0) {
			value.assign(buffer.data());
		}
	}

	while (value.size() > 3 && IsDirectorySeparator(value[value.size() - 1])) {
		value.erase(value.size() - 1);
	}
	return value;
}

static std::wstring GetEnvironmentString(const wchar_t* name)
{
	DWORD length = GetEnvironmentVariableW(name, NULL, 0);
	if (length == 0) return std::wstring();

	std::vector<wchar_t> buffer(length);
	if (GetEnvironmentVariableW(name, buffer.data(), length) == 0) {
		return std::wstring();
	}
	return Trim(buffer.data());
}

static bool FileExists(const std::wstring& path)
{
	DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool DirectoryExists(const std::wstring& path)
{
	DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool EnsureDirectoryExists(const std::wstring& path)
{
	if (path.empty()) return false;
	if (DirectoryExists(path)) return true;
	if (path.size() == 2 && path[1] == L':') return true;
	if (path.size() == 3 && path[1] == L':' && IsDirectorySeparator(path[2])) return true;

	std::wstring parent = GetDirectoryName(path);
	if (!parent.empty() && parent != path && !EnsureDirectoryExists(parent)) {
		return false;
	}

	if (CreateDirectoryW(path.c_str(), NULL)) {
		return true;
	}

	DWORD error = GetLastError();
	return error == ERROR_ALREADY_EXISTS && DirectoryExists(path);
}

static bool DeleteFileIfExists(const std::wstring& path)
{
	if (!FileExists(path)) return true;
	return DeleteFileW(path.c_str()) != FALSE;
}

static bool ReadBinaryFile(const std::wstring& path, std::string& out_data)
{
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}

	LARGE_INTEGER file_size;
	if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart < 0 || file_size.QuadPart > 0x7fffffff) {
		CloseHandle(file);
		return false;
	}

	std::string data;
	data.resize(static_cast<size_t>(file_size.QuadPart));

	DWORD bytes_read = 0;
	BOOL ok = TRUE;
	if (!data.empty()) {
		ok = ReadFile(file, &data[0], static_cast<DWORD>(data.size()), &bytes_read, NULL);
	}
	CloseHandle(file);

	if (!ok || bytes_read != data.size()) {
		return false;
	}

	out_data.swap(data);
	return true;
}

static bool WriteBinaryFileAtomic(const std::wstring& path, const std::string& data)
{
	if (!EnsureDirectoryExists(GetDirectoryName(path))) {
		return false;
	}

	std::wstring temp_path = path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
	HANDLE file = CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}

	DWORD bytes_written = 0;
	BOOL ok = TRUE;
	if (!data.empty()) {
		ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &bytes_written, NULL);
	}
	if (ok) {
		ok = FlushFileBuffers(file);
	}
	CloseHandle(file);

	if (!ok || bytes_written != data.size()) {
		DeleteFileW(temp_path.c_str());
		return false;
	}

	if (!MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
		DeleteFileW(temp_path.c_str());
		return false;
	}

	return true;
}

static bool WriteUtf8TextFileAtomic(const std::wstring& path, const std::string& text)
{
	return WriteBinaryFileAtomic(path, text);
}

static bool LoadResourceBytes(HINSTANCE instance, int resource_id, std::string& out_data)
{
	HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resource_id), reinterpret_cast<LPCWSTR>(RT_RCDATA));
	if (resource == NULL) return false;

	DWORD resource_size = SizeofResource(instance, resource);
	if (resource_size == 0) return false;

	HGLOBAL handle = LoadResource(instance, resource);
	if (handle == NULL) return false;

	const void* locked = LockResource(handle);
	if (locked == NULL) return false;

	out_data.assign(static_cast<const char*>(locked), static_cast<const char*>(locked) + resource_size);
	return true;
}

static std::wstring Utf8ToWide(const std::string& value)
{
	if (value.empty()) return std::wstring();
	int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), NULL, 0);
	if (length <= 0) return std::wstring();
	std::wstring wide(length, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &wide[0], length);
	return wide;
}

static std::string WideToUtf8(const std::wstring& value)
{
	if (value.empty()) return std::string();
	int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), NULL, 0, NULL, NULL);
	if (length <= 0) return std::string();
	std::string utf8(length, '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &utf8[0], length, NULL, NULL);
	return utf8;
}

static std::string JsonEscape(const std::string& value)
{
	std::string result;
	result.reserve(value.size() + 16);
	for (size_t i = 0; i < value.size(); ++i) {
		unsigned char ch = static_cast<unsigned char>(value[i]);
		switch (ch) {
		case '\\':
			result += "\\\\";
			break;
		case '"':
			result += "\\\"";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (ch < 0x20) {
				char buffer[8];
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
				result += buffer;
			}
			else {
				result.push_back(static_cast<char>(ch));
			}
			break;
		}
	}
	return result;
}

static bool ParseJsonStringLiteral(const std::string& text, size_t start_quote, std::string& out_value, size_t* end_pos)
{
	if (start_quote >= text.size() || text[start_quote] != '"') {
		return false;
	}

	std::string value;
	size_t pos = start_quote + 1;
	while (pos < text.size()) {
		char ch = text[pos++];
		if (ch == '"') {
			out_value.swap(value);
			if (end_pos != NULL) {
				*end_pos = pos;
			}
			return true;
		}
		if (ch != '\\') {
			value.push_back(ch);
			continue;
		}
		if (pos >= text.size()) return false;
		char escaped = text[pos++];
		switch (escaped) {
		case '\\': value.push_back('\\'); break;
		case '"': value.push_back('"'); break;
		case 'r': value.push_back('\r'); break;
		case 'n': value.push_back('\n'); break;
		case 't': value.push_back('\t'); break;
		case 'u':
			if (pos + 4 > text.size()) return false;
			if (text.compare(pos, 4, "000a") == 0) value.push_back('\n');
			else if (text.compare(pos, 4, "000d") == 0) value.push_back('\r');
			else if (text.compare(pos, 4, "0009") == 0) value.push_back('\t');
			pos += 4;
			break;
		default:
			value.push_back(escaped);
			break;
		}
	}
	return false;
}

static bool ExtractJsonStringField(const std::string& text, const std::string& key, size_t search_start, std::string& out_value)
{
	const std::string marker = "\"" + key + "\"";
	size_t key_pos = text.find(marker, search_start);
	if (key_pos == std::string::npos) return false;

	size_t colon_pos = text.find(':', key_pos + marker.size());
	if (colon_pos == std::string::npos) return false;

	size_t quote_pos = text.find('"', colon_pos + 1);
	if (quote_pos == std::string::npos) return false;

	size_t end_pos = 0;
	return ParseJsonStringLiteral(text, quote_pos, out_value, &end_pos);
}

static bool ExtractJsonArraySegment(const std::string& text, const std::string& key, std::string& out_segment)
{
	const std::string marker = "\"" + key + "\"";
	size_t key_pos = text.find(marker);
	if (key_pos == std::string::npos) return false;

	size_t bracket_pos = text.find('[', key_pos + marker.size());
	if (bracket_pos == std::string::npos) return false;

	int depth = 0;
	bool in_string = false;
	for (size_t pos = bracket_pos; pos < text.size(); ++pos) {
		char ch = text[pos];
		if (in_string) {
			if (ch == '\\') {
				++pos;
			}
			else if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		if (ch == '"') {
			in_string = true;
			continue;
		}
		if (ch == '[') {
			++depth;
			continue;
		}
		if (ch == ']') {
			--depth;
			if (depth == 0) {
				out_segment = text.substr(bracket_pos + 1, pos - bracket_pos - 1);
				return true;
			}
		}
	}

	return false;
}

static void ParseJsonObjectSegments(const std::string& array_segment, std::vector<std::string>& out_objects)
{
	out_objects.clear();
	int depth = 0;
	bool in_string = false;
	size_t object_start = std::string::npos;
	for (size_t pos = 0; pos < array_segment.size(); ++pos) {
		char ch = array_segment[pos];
		if (in_string) {
			if (ch == '\\') {
				++pos;
			}
			else if (ch == '"') {
				in_string = false;
			}
			continue;
		}

		if (ch == '"') {
			in_string = true;
			continue;
		}
		if (ch == '{') {
			if (depth == 0) {
				object_start = pos;
			}
			++depth;
			continue;
		}
		if (ch == '}') {
			--depth;
			if (depth == 0 && object_start != std::string::npos) {
				out_objects.push_back(array_segment.substr(object_start, pos - object_start + 1));
				object_start = std::string::npos;
			}
		}
	}
}

static std::string ComputeHashHex(const std::string& data)
{
	unsigned long long hash = 1469598103934665603ULL;
	for (size_t i = 0; i < data.size(); ++i) {
		hash ^= static_cast<unsigned char>(data[i]);
		hash *= 1099511628211ULL;
	}

	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%016llx", hash);
	return buffer;
}

static std::wstring GetWindowTextString(HWND handle)
{
	int length = GetWindowTextLengthW(handle);
	std::wstring text(length + 1, L'\0');
	if (length > 0) {
		GetWindowTextW(handle, &text[0], length + 1);
	}
	text.resize(length);
	return text;
}

static void AppendStatus(const std::wstring& line)
{
	if (g_app == NULL || g_app->StatusEdit == NULL) return;

	std::wstring current = GetWindowTextString(g_app->StatusEdit);
	if (!current.empty()) current += L"\r\n";
	current += line;
	SetWindowTextW(g_app->StatusEdit, current.c_str());
	SendMessageW(g_app->StatusEdit, EM_SETSEL, current.size(), current.size());
	SendMessageW(g_app->StatusEdit, EM_SCROLLCARET, 0, 0);
}

static void ClearStatus()
{
	if (g_app != NULL && g_app->StatusEdit != NULL) {
		SetWindowTextW(g_app->StatusEdit, L"");
	}
}

static bool IsAsciiSpace(char ch)
{
	return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static bool ParseWidgetTag(const std::string& tag_text, XmlWidgetTag& out_tag)
{
	size_t pos = tag_text.find("<widget");
	if (pos == std::string::npos) return false;
	pos += 7;

	while (pos < tag_text.size()) {
		while (pos < tag_text.size() && IsAsciiSpace(tag_text[pos])) ++pos;
		if (pos >= tag_text.size() || tag_text[pos] == '/' || tag_text[pos] == '>') break;

		size_t name_start = pos;
		while (pos < tag_text.size() && !IsAsciiSpace(tag_text[pos]) && tag_text[pos] != '=' && tag_text[pos] != '/' && tag_text[pos] != '>') ++pos;
		if (pos == name_start) return false;

		std::string name = tag_text.substr(name_start, pos - name_start);
		while (pos < tag_text.size() && IsAsciiSpace(tag_text[pos])) ++pos;
		if (pos >= tag_text.size() || tag_text[pos] != '=') return false;
		++pos;

		while (pos < tag_text.size() && IsAsciiSpace(tag_text[pos])) ++pos;
		if (pos >= tag_text.size() || tag_text[pos] != '"') return false;

		std::string value;
		size_t end_pos = 0;
		if (!ParseJsonStringLiteral(tag_text, pos, value, &end_pos)) return false;
		pos = end_pos;

		out_tag.Order.push_back(name);
		out_tag.Attributes[name] = value;
	}

	return !out_tag.Attributes.empty();
}

static void SetWidgetAttribute(XmlWidgetTag& tag, const std::string& name, const std::string& value, bool overwrite)
{
	std::map<std::string, std::string>::iterator it = tag.Attributes.find(name);
	if (it == tag.Attributes.end()) {
		tag.Order.push_back(name);
		tag.Attributes[name] = value;
		return;
	}

	if (overwrite) {
		it->second = value;
	}
}

static std::string BuildWidgetTagString(const XmlWidgetTag& tag)
{
	std::string result = "<widget";
	for (size_t i = 0; i < tag.Order.size(); ++i) {
		std::map<std::string, std::string>::const_iterator it = tag.Attributes.find(tag.Order[i]);
		if (it == tag.Attributes.end()) continue;
		result += " ";
		result += it->first;
		result += "=\"";
		result += it->second;
		result += "\"";
	}
	result += " />";
	return result;
}

static std::string DetectLineEnding(const std::string& text)
{
	return text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
}

static std::string DetectWidgetIndent(const std::string& text)
{
	size_t widget_pos = text.find("<widget");
	if (widget_pos == std::string::npos) return "  ";

	size_t line_start = text.rfind('\n', widget_pos);
	if (line_start == std::string::npos) line_start = 0;
	else ++line_start;

	size_t pos = line_start;
	while (pos < widget_pos && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
	return text.substr(line_start, pos - line_start);
}

static std::wstring GetFileNameOnly(const std::wstring& path)
{
	size_t pos = path.find_last_of(L"\\/");
	if (pos == std::wstring::npos) return path;
	return path.substr(pos + 1);
}

static LayoutPatchKind GetLayoutPatchKind(const std::wstring& path)
{
	std::wstring filename = ToLowerWide(GetFileNameOnly(path));
	if (filename == L"default.xml") return LayoutPatchKind::Default;
	if (filename == L"standard_icon.xml") return LayoutPatchKind::StandardIcon;
	if (filename == L"basic.xml") return LayoutPatchKind::Basic;
	if (filename == L"mapping.xml") return LayoutPatchKind::Mapping;
	return LayoutPatchKind::Unknown;
}

static bool FindWidgetTagRange(const std::string& text, const std::string& widget_name, size_t& tag_start, size_t& tag_end)
{
	const std::string marker = "name=\"" + widget_name + "\"";
	size_t name_pos = text.find(marker);
	if (name_pos == std::string::npos) {
		return false;
	}

	tag_start = text.rfind("<widget", name_pos);
	size_t close_pos = text.find("/>", name_pos);
	if (tag_start == std::string::npos || close_pos == std::string::npos || close_pos < tag_start) {
		return false;
	}

	tag_end = close_pos + 2;
	return true;
}

static void ConfigureWidgetAttributes(LayoutPatchKind kind, InstallAction action, XmlWidgetTag& tag, bool existing_widget)
{
	const bool overwrite = action == InstallAction::Repair || kind != LayoutPatchKind::Basic;
	switch (kind) {
	case LayoutPatchKind::Default:
		SetWidgetAttribute(tag, "parent", "FrameComMiscButtons", true);
		break;
	case LayoutPatchKind::StandardIcon:
		SetWidgetAttribute(tag, "visible", "true", true);
		SetWidgetAttribute(tag, "horz", "hintsize", true);
		SetWidgetAttribute(tag, "vert", "hintsize", true);
		SetWidgetAttribute(tag, "hintsize_x", "2.5", true);
		SetWidgetAttribute(tag, "hintsize_y", "3.0", true);
		SetWidgetAttribute(tag, "font_scale", "0.8", true);
		SetWidgetAttribute(tag, "icon", "cmd_insetface.svg", true);
		SetWidgetAttribute(tag, "icon_scale", "1.8", true);
		SetWidgetAttribute(tag, "chain", "true", true);
		SetWidgetAttribute(tag, "image_pos", "top", true);
		break;
	case LayoutPatchKind::Basic:
		if (!existing_widget || action == InstallAction::Repair) {
			SetWidgetAttribute(tag, "visible", "false", overwrite);
		}
		break;
	case LayoutPatchKind::Mapping:
		SetWidgetAttribute(tag, "visible", "false", true);
		break;
	case LayoutPatchKind::Unknown:
	default:
		break;
	}
}

static bool PatchLayoutText(std::string& text, LayoutPatchKind kind, InstallAction action, LayoutManifestEntry& manifest_entry)
{
	if (kind == LayoutPatchKind::Unknown || action == InstallAction::Uninstall) return false;

	size_t tag_start = 0;
	size_t tag_end = 0;
	if (FindWidgetTagRange(text, kWidgetName, tag_start, tag_end)) {
		std::string old_tag = text.substr(tag_start, tag_end - tag_start);
		manifest_entry.OriginalTag = old_tag;

		if (kind == LayoutPatchKind::Basic && action == InstallAction::Install) {
			manifest_entry.Action = "unchanged";
			return true;
		}

		XmlWidgetTag tag;
		if (!ParseWidgetTag(old_tag, tag)) return false;

		ConfigureWidgetAttributes(kind, action, tag, true);
		std::string new_tag = BuildWidgetTagString(tag);
		if (new_tag != old_tag) {
			text.replace(tag_start, old_tag.size(), new_tag);
			manifest_entry.Action = "updated";
		}
		else {
			manifest_entry.Action = "unchanged";
		}
		return true;
	}

	XmlWidgetTag new_tag;
	new_tag.Order.push_back("name");
	new_tag.Attributes["name"] = kWidgetName;
	ConfigureWidgetAttributes(kind, action, new_tag, false);

	size_t layout_end = text.rfind("</layout>");
	if (layout_end == std::string::npos) return false;

	std::string newline = DetectLineEnding(text);
	std::string indent = DetectWidgetIndent(text);
	std::string insertion;
	if (layout_end > 0 && text[layout_end - 1] != '\n' && text[layout_end - 1] != '\r') {
		insertion += newline;
	}
	insertion += indent;
	insertion += BuildWidgetTagString(new_tag);
	insertion += newline;
	text.insert(layout_end, insertion);

	manifest_entry.Action = "inserted";
	manifest_entry.OriginalTag.clear();
	return true;
}

static bool RevertLayoutText(std::string& text, const LayoutManifestEntry& manifest_entry)
{
	size_t tag_start = 0;
	size_t tag_end = 0;
	if (!FindWidgetTagRange(text, kWidgetName, tag_start, tag_end)) {
		return true;
	}

	if (manifest_entry.Action == "inserted" || manifest_entry.OriginalTag.empty()) {
		size_t line_start = text.rfind('\n', tag_start);
		if (line_start == std::string::npos) {
			line_start = 0;
		}
		else {
			++line_start;
		}

		size_t line_end = text.find('\n', tag_end);
		if (line_end == std::string::npos) {
			line_end = text.size();
		}
		else {
			++line_end;
		}
		text.erase(line_start, line_end - line_start);
		return true;
	}

	text.replace(tag_start, tag_end - tag_start, manifest_entry.OriginalTag);
	return true;
}

static bool ValidateMetasequoiaRoot(const std::wstring& root)
{
	if (root.empty()) return false;
	return FileExists(JoinPath(root, L"Metaseq.exe")) &&
		DirectoryExists(JoinPath(root, L"Plugins\\Command")) &&
		DirectoryExists(JoinPath(root, L"Data\\Layout")) &&
		DirectoryExists(JoinPath(root, L"Data\\Icon\\White")) &&
		DirectoryExists(JoinPath(root, L"Data\\Icon\\Black"));
}

static bool IsMetasequoiaRunning()
{
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return false;
	}

	PROCESSENTRY32W entry;
	entry.dwSize = sizeof(entry);
	BOOL ok = Process32FirstW(snapshot, &entry);
	while (ok) {
		if (_wcsicmp(entry.szExeFile, kProcessName) == 0) {
			CloseHandle(snapshot);
			return true;
		}
		ok = Process32NextW(snapshot, &entry);
	}

	CloseHandle(snapshot);
	return false;
}

static bool EnsureMetasequoiaClosed(HWND owner)
{
	while (IsMetasequoiaRunning()) {
		int result = MessageBoxW(owner,
			L"Metasequoia is running.\r\n\r\nClose Metasequoia, then click Retry to continue.",
			kAppTitle,
			MB_ICONWARNING | MB_RETRYCANCEL);
		if (result != IDRETRY) {
			return false;
		}
	}
	return true;
}

static bool QueryRegistryString(HKEY root, const std::wstring& subkey, const wchar_t* value_name, std::wstring& out_value)
{
	HKEY key = NULL;
	if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
		return false;
	}

	DWORD type = 0;
	DWORD size = 0;
	LONG result = RegQueryValueExW(key, value_name, NULL, &type, NULL, &size);
	if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0) {
		RegCloseKey(key);
		return false;
	}

	std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
	result = RegQueryValueExW(key, value_name, NULL, &type, reinterpret_cast<LPBYTE>(buffer.data()), &size);
	RegCloseKey(key);
	if (result != ERROR_SUCCESS) {
		return false;
	}

	out_value.assign(buffer.data());
	out_value = Trim(out_value);
	return !out_value.empty();
}

static bool TryCandidateRoot(const std::wstring& candidate, std::wstring& out_root)
{
	std::wstring normalized = NormalizePath(candidate);
	if (!ValidateMetasequoiaRoot(normalized)) {
		return false;
	}
	out_root = normalized;
	return true;
}

static bool LooksLikeMetasequoiaDisplayName(const std::wstring& display_name)
{
	if (display_name.empty()) return false;
	return ContainsInsensitive(display_name, L"Metasequoia") || ContainsInsensitive(display_name, L"Metaseq");
}

static bool FindMetasequoiaRootFromAppPath(HKEY root, std::wstring& out_root)
{
	std::wstring candidate;
	if (QueryRegistryString(root, kRegistryAppPaths, NULL, candidate) && TryCandidateRoot(candidate, out_root)) {
		return true;
	}
	if (QueryRegistryString(root, kRegistryAppPaths, L"Path", candidate) && TryCandidateRoot(candidate, out_root)) {
		return true;
	}
	return false;
}

static bool FindMetasequoiaRootInRegistryKey(HKEY root, const std::wstring& base_subkey, std::wstring& out_root)
{
	HKEY base_key = NULL;
	if (RegOpenKeyExW(root, base_subkey.c_str(), 0, KEY_READ, &base_key) != ERROR_SUCCESS) {
		return false;
	}

	DWORD index = 0;
	wchar_t name_buffer[256];
	DWORD name_length = 256;
	while (RegEnumKeyExW(base_key, index, name_buffer, &name_length, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
		std::wstring subkey_name(name_buffer, name_length);
		std::wstring full_subkey = base_subkey + L"\\" + subkey_name;
		std::wstring display_name;
		if (QueryRegistryString(root, full_subkey, L"DisplayName", display_name)) {
			if (LooksLikeMetasequoiaDisplayName(display_name)) {
				std::wstring candidate;
				if (!QueryRegistryString(root, full_subkey, L"InstallLocation", candidate)) {
					QueryRegistryString(root, full_subkey, L"DisplayIcon", candidate);
				}
				if (TryCandidateRoot(candidate, out_root)) {
					RegCloseKey(base_key);
					return true;
				}
			}
		}
		++index;
		name_length = 256;
	}

	RegCloseKey(base_key);
	return false;
}

static bool FindMetasequoiaRootInKnownLocations(std::wstring& out_root)
{
	std::vector<std::wstring> candidates;
	std::wstring program_files = GetEnvironmentString(L"ProgramFiles");
	std::wstring program_files_x86 = GetEnvironmentString(L"ProgramFiles(x86)");

	if (!program_files.empty()) {
		candidates.push_back(JoinPath(program_files, L"tetraface\\Metasequoia4"));
		candidates.push_back(JoinPath(program_files, L"Metasequoia4"));
	}
	if (!program_files_x86.empty()) {
		candidates.push_back(JoinPath(program_files_x86, L"tetraface\\Metasequoia4"));
		candidates.push_back(JoinPath(program_files_x86, L"Metasequoia4"));
	}

	candidates.push_back(L"D:\\software\\Metasequoia4");

	for (size_t i = 0; i < candidates.size(); ++i) {
		if (TryCandidateRoot(candidates[i], out_root)) {
			return true;
		}
	}
	return false;
}

static std::wstring AutoDetectMetasequoiaRoot()
{
	std::wstring root;
	if (FindMetasequoiaRootFromAppPath(HKEY_CURRENT_USER, root)) {
		return root;
	}
	if (FindMetasequoiaRootFromAppPath(HKEY_LOCAL_MACHINE, root)) {
		return root;
	}
	if (FindMetasequoiaRootInRegistryKey(HKEY_CURRENT_USER, kRegistryUninstall, root)) {
		return root;
	}
	if (FindMetasequoiaRootInRegistryKey(HKEY_CURRENT_USER, kRegistryUninstallWow6432, root)) {
		return root;
	}
	if (FindMetasequoiaRootInRegistryKey(HKEY_LOCAL_MACHINE, kRegistryUninstall, root)) {
		return root;
	}
	if (FindMetasequoiaRootInRegistryKey(HKEY_LOCAL_MACHINE, kRegistryUninstallWow6432, root)) {
		return root;
	}
	if (FindMetasequoiaRootInKnownLocations(root)) {
		return root;
	}
	return std::wstring();
}

static bool LoadManifest(const std::wstring& path, InstallManifest& out_manifest)
{
	std::string json;
	if (!ReadBinaryFile(path, json)) {
		return false;
	}

	std::string dll_path;
	std::string white_icon_path;
	std::string black_icon_path;
	if (ExtractJsonStringField(json, "dllPath", 0, dll_path)) {
		out_manifest.DllPath = Utf8ToWide(dll_path);
	}
	if (ExtractJsonStringField(json, "whiteIconPath", 0, white_icon_path)) {
		out_manifest.WhiteIconPath = Utf8ToWide(white_icon_path);
	}
	if (ExtractJsonStringField(json, "blackIconPath", 0, black_icon_path)) {
		out_manifest.BlackIconPath = Utf8ToWide(black_icon_path);
	}
	ExtractJsonStringField(json, "dllHash", 0, out_manifest.DllHash);
	ExtractJsonStringField(json, "whiteIconHash", 0, out_manifest.WhiteIconHash);
	ExtractJsonStringField(json, "blackIconHash", 0, out_manifest.BlackIconHash);

	std::string layouts_segment;
	if (ExtractJsonArraySegment(json, "layouts", layouts_segment)) {
		std::vector<std::string> objects;
		ParseJsonObjectSegments(layouts_segment, objects);
		for (size_t i = 0; i < objects.size(); ++i) {
			LayoutManifestEntry entry;
			std::string value;
			if (ExtractJsonStringField(objects[i], "name", 0, value)) entry.Name = Utf8ToWide(value);
			if (ExtractJsonStringField(objects[i], "path", 0, value)) entry.Path = Utf8ToWide(value);
			if (ExtractJsonStringField(objects[i], "action", 0, value)) entry.Action = value;
			if (ExtractJsonStringField(objects[i], "originalTag", 0, value)) entry.OriginalTag = value;
			out_manifest.Layouts.push_back(entry);
		}
	}

	return true;
}

static std::string BuildManifestJson(const InstallManifest& manifest)
{
	std::string json;
	json += "{\n";
	json += "  \"version\": ";
	json += kInstallerVersion;
	json += ",\n";
	json += "  \"productId\": \"56A31D20\",\n";
	json += "  \"pluginId\": \"5A1E7F44\",\n";
	json += "  \"dllPath\": \"";
	json += JsonEscape(WideToUtf8(manifest.DllPath));
	json += "\",\n";
	json += "  \"whiteIconPath\": \"";
	json += JsonEscape(WideToUtf8(manifest.WhiteIconPath));
	json += "\",\n";
	json += "  \"blackIconPath\": \"";
	json += JsonEscape(WideToUtf8(manifest.BlackIconPath));
	json += "\",\n";
	json += "  \"dllHash\": \"";
	json += manifest.DllHash;
	json += "\",\n";
	json += "  \"whiteIconHash\": \"";
	json += manifest.WhiteIconHash;
	json += "\",\n";
	json += "  \"blackIconHash\": \"";
	json += manifest.BlackIconHash;
	json += "\",\n";
	json += "  \"layouts\": [\n";
	for (size_t i = 0; i < manifest.Layouts.size(); ++i) {
		const LayoutManifestEntry& entry = manifest.Layouts[i];
		json += "    {\n";
		json += "      \"name\": \"";
		json += JsonEscape(WideToUtf8(entry.Name));
		json += "\",\n";
		json += "      \"path\": \"";
		json += JsonEscape(WideToUtf8(entry.Path));
		json += "\",\n";
		json += "      \"action\": \"";
		json += JsonEscape(entry.Action);
		json += "\",\n";
		json += "      \"originalTag\": \"";
		json += JsonEscape(entry.OriginalTag);
		json += "\"\n";
		json += "    }";
		if (i + 1 < manifest.Layouts.size()) {
			json += ",";
		}
		json += "\n";
	}
	json += "  ]\n";
	json += "}\n";
	return json;
}

static bool ApplyPayloadFile(HINSTANCE instance, int resource_id, const std::wstring& target_path, std::string* out_hash)
{
	std::string data;
	if (!LoadResourceBytes(instance, resource_id, data)) {
		return false;
	}
	if (!WriteBinaryFileAtomic(target_path, data)) {
		return false;
	}
	if (out_hash != NULL) {
		*out_hash = ComputeHashHex(data);
	}
	return true;
}

static bool ApplyLayoutPatchFile(const std::wstring& layout_path, InstallAction action, LayoutManifestEntry& manifest_entry)
{
	std::string original_text;
	if (!ReadBinaryFile(layout_path, original_text)) {
		return false;
	}

	LayoutPatchKind kind = GetLayoutPatchKind(layout_path);
	if (kind == LayoutPatchKind::Unknown) {
		return false;
	}

	std::string patched_text = original_text;
	if (!PatchLayoutText(patched_text, kind, action, manifest_entry)) {
		return false;
	}

	if (patched_text != original_text && !WriteUtf8TextFileAtomic(layout_path, patched_text)) {
		return false;
	}

	return true;
}

static bool RevertLayoutPatchFile(const LayoutManifestEntry& manifest_entry)
{
	std::string text;
	if (!ReadBinaryFile(manifest_entry.Path, text)) {
		return false;
	}

	std::string updated_text = text;
	if (!RevertLayoutText(updated_text, manifest_entry)) {
		return false;
	}

	if (updated_text != text && !WriteUtf8TextFileAtomic(manifest_entry.Path, updated_text)) {
		return false;
	}

	return true;
}

static void CollectDefaultLayoutEntries(const std::wstring& root, std::vector<LayoutManifestEntry>& out_entries)
{
	static const wchar_t* kLayoutNames[] = {
		L"default.xml",
		L"standard_icon.xml",
		L"basic.xml",
		L"mapping.xml",
	};

	out_entries.clear();
	std::wstring layout_root = JoinPath(root, L"Data\\Layout");
	for (size_t i = 0; i < sizeof(kLayoutNames) / sizeof(kLayoutNames[0]); ++i) {
		LayoutManifestEntry entry;
		entry.Name = kLayoutNames[i];
		entry.Path = JoinPath(layout_root, kLayoutNames[i]);
		entry.Action = "inserted";
		out_entries.push_back(entry);
	}
}

static bool RunInstallOrRepair(const std::wstring& root, InstallAction action)
{
	std::wstring plugin_path = JoinPath(root, L"Plugins\\Command\\" + std::wstring(kPluginFileName));
	std::wstring resource_path = JoinPath(root, L"Plugins\\Command\\" + std::wstring(kResourceFileName));
	std::wstring white_icon_path = JoinPath(root, L"Data\\Icon\\White\\" + std::wstring(kIconFileName));
	std::wstring black_icon_path = JoinPath(root, L"Data\\Icon\\Black\\" + std::wstring(kIconFileName));
	std::wstring manifest_path = JoinPath(root, L"Plugins\\Command\\" + std::wstring(kManifestFileName));

	AppendStatus(L"Deploying plugin DLL...");
	InstallManifest manifest;
	manifest.DllPath = plugin_path;
	manifest.WhiteIconPath = white_icon_path;
	manifest.BlackIconPath = black_icon_path;

	if (!ApplyPayloadFile(g_app->Instance, IDR_PAYLOAD_DLL, plugin_path, &manifest.DllHash)) {
		AppendStatus(L"Failed to write InsetFace.dll.");
		return false;
	}

	AppendStatus(L"Deploying language resource...");
	if (!ApplyPayloadFile(g_app->Instance, IDR_PAYLOAD_RESOURCE_XML, resource_path, NULL)) {
		AppendStatus(L"Failed to write InsetFace.resource.xml.");
		return false;
	}

	AppendStatus(L"Deploying white-theme icon...");
	if (!ApplyPayloadFile(g_app->Instance, IDR_PAYLOAD_ICON_WHITE, white_icon_path, &manifest.WhiteIconHash)) {
		AppendStatus(L"Failed to write Data\\Icon\\White\\cmd_insetface.svg.");
		return false;
	}

	AppendStatus(L"Deploying dark-theme icon...");
	if (!ApplyPayloadFile(g_app->Instance, IDR_PAYLOAD_ICON_BLACK, black_icon_path, &manifest.BlackIconHash)) {
		AppendStatus(L"Failed to write Data\\Icon\\Black\\cmd_insetface.svg.");
		return false;
	}

	std::vector<LayoutManifestEntry> layouts;
	CollectDefaultLayoutEntries(root, layouts);
	for (size_t i = 0; i < layouts.size(); ++i) {
		AppendStatus(L"Patching " + layouts[i].Name + L"...");
		if (!ApplyLayoutPatchFile(layouts[i].Path, action, layouts[i])) {
			AppendStatus(L"Failed to patch " + layouts[i].Name + L".");
			return false;
		}
		manifest.Layouts.push_back(layouts[i]);
	}

	AppendStatus(L"Writing installer manifest...");
	if (!WriteUtf8TextFileAtomic(manifest_path, BuildManifestJson(manifest))) {
		AppendStatus(L"Failed to write installer manifest.");
		return false;
	}

	return true;
}

static bool RunUninstall(const std::wstring& root)
{
	std::wstring manifest_path = JoinPath(root, L"Plugins\\Command\\" + std::wstring(kManifestFileName));
	std::wstring resource_path = JoinPath(root, L"Plugins\\Command\\" + std::wstring(kResourceFileName));
	InstallManifest manifest;
	if (!LoadManifest(manifest_path, manifest)) {
		manifest.DllPath = JoinPath(root, L"Plugins\\Command\\" + std::wstring(kPluginFileName));
		manifest.WhiteIconPath = JoinPath(root, L"Data\\Icon\\White\\" + std::wstring(kIconFileName));
		manifest.BlackIconPath = JoinPath(root, L"Data\\Icon\\Black\\" + std::wstring(kIconFileName));
		CollectDefaultLayoutEntries(root, manifest.Layouts);
	}

	for (size_t i = 0; i < manifest.Layouts.size(); ++i) {
		if (!FileExists(manifest.Layouts[i].Path)) {
			continue;
		}
		AppendStatus(L"Reverting " + manifest.Layouts[i].Name + L"...");
		if (!RevertLayoutPatchFile(manifest.Layouts[i])) {
			AppendStatus(L"Failed to revert " + manifest.Layouts[i].Name + L".");
			return false;
		}
	}

	AppendStatus(L"Removing installed files...");
	if (!manifest.DllPath.empty() && !DeleteFileIfExists(manifest.DllPath)) {
		AppendStatus(L"Failed to remove InsetFace.dll.");
		return false;
	}
	if (!manifest.WhiteIconPath.empty() && !DeleteFileIfExists(manifest.WhiteIconPath)) {
		AppendStatus(L"Failed to remove white-theme icon.");
		return false;
	}
	if (!manifest.BlackIconPath.empty() && !DeleteFileIfExists(manifest.BlackIconPath)) {
		AppendStatus(L"Failed to remove dark-theme icon.");
		return false;
	}
	if (!DeleteFileIfExists(resource_path)) {
		AppendStatus(L"Failed to remove InsetFace.resource.xml.");
		return false;
	}

	DeleteFileIfExists(manifest_path);
	return true;
}

static std::wstring ReadSelectedRoot()
{
	if (g_app == NULL || g_app->PathEdit == NULL) return std::wstring();
	return NormalizePath(GetWindowTextString(g_app->PathEdit));
}

static void RefreshButtons()
{
	if (g_app == NULL) return;
	std::wstring root = ReadSelectedRoot();
	const bool valid_root = ValidateMetasequoiaRoot(root);
	const bool installed = valid_root && (FileExists(JoinPath(root, L"Plugins\\Command\\" + std::wstring(kPluginFileName))) ||
		FileExists(JoinPath(root, L"Plugins\\Command\\" + std::wstring(kManifestFileName))));

	EnableWindow(g_app->InstallButton, valid_root ? TRUE : FALSE);
	EnableWindow(g_app->RepairButton, valid_root ? TRUE : FALSE);
	EnableWindow(g_app->UninstallButton, (valid_root && installed) ? TRUE : FALSE);
}

static void SetBusy(bool busy)
{
	if (g_app == NULL) return;
	EnableWindow(g_app->PathEdit, busy ? FALSE : TRUE);
	EnableWindow(g_app->BrowseButton, busy ? FALSE : TRUE);
	EnableWindow(g_app->InstallButton, busy ? FALSE : TRUE);
	EnableWindow(g_app->RepairButton, busy ? FALSE : TRUE);
	EnableWindow(g_app->UninstallButton, busy ? FALSE : TRUE);
	SetCursor(LoadCursorW(NULL, busy ? IDC_WAIT : IDC_ARROW));
}

static void ChooseMetaseqExecutable(HWND owner)
{
	wchar_t file_buffer[MAX_PATH] = L"";
	OPENFILENAMEW dialog = {};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = owner;
	dialog.lpstrFilter = L"Metaseq executable\0Metaseq.exe\0Executable files\0*.exe\0All files\0*.*\0";
	dialog.lpstrFile = file_buffer;
	dialog.nMaxFile = MAX_PATH;
	dialog.lpstrTitle = L"Choose Metaseq.exe";
	dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	dialog.lpstrDefExt = L"exe";

	if (!GetOpenFileNameW(&dialog)) {
		return;
	}

	if (g_app == NULL || g_app->PathEdit == NULL) {
		return;
	}

	std::wstring root = NormalizePath(file_buffer);
	SetWindowTextW(g_app->PathEdit, root.c_str());
	RefreshButtons();
}

static void RunAction(InstallAction action)
{
	std::wstring root = ReadSelectedRoot();
	if (!ValidateMetasequoiaRoot(root)) {
		MessageBoxW(g_app->Window, L"Choose a valid Metasequoia install directory first.", kAppTitle, MB_ICONWARNING | MB_OK);
		return;
	}

	if (!EnsureMetasequoiaClosed(g_app->Window)) {
		return;
	}

	ClearStatus();
	SetBusy(true);

	bool ok = false;
	if (action == InstallAction::Install) {
		AppendStatus(L"Running install/update...");
		ok = RunInstallOrRepair(root, action);
	}
	else if (action == InstallAction::Repair) {
		AppendStatus(L"Running repair...");
		ok = RunInstallOrRepair(root, action);
	}
	else {
		AppendStatus(L"Running uninstall...");
		ok = RunUninstall(root);
	}

	SetBusy(false);
	RefreshButtons();

	if (ok) {
		if (action == InstallAction::Install) {
			AppendStatus(L"Install/update completed successfully.");
		}
		else if (action == InstallAction::Repair) {
			AppendStatus(L"Repair completed successfully.");
		}
		else {
			AppendStatus(L"Uninstall completed successfully.");
		}
	}
	else {
		AppendStatus(L"Operation did not complete.");
	}
}

static HWND CreateAppControl(DWORD style, DWORD ex_style, const wchar_t* class_name, const wchar_t* text, int x, int y, int width, int height, int control_id)
{
	HWND handle = CreateWindowExW(
		ex_style,
		class_name,
		text,
		style,
		x,
		y,
		width,
		height,
		g_app->Window,
		reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
		g_app->Instance,
		NULL);
	SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(g_app->UiFont), TRUE);
	return handle;
}

static HFONT CreateUiFont(int height, int weight, bool italic, const wchar_t* face_name)
{
	LOGFONTW font = {};
	font.lfHeight = height;
	font.lfWeight = weight;
	font.lfItalic = italic ? TRUE : FALSE;
	font.lfQuality = CLEARTYPE_QUALITY;
	wcscpy_s(font.lfFaceName, face_name);
	return CreateFontIndirectW(&font);
}

static bool IsPrimaryButton(HWND handle)
{
	return g_app != NULL && handle == g_app->InstallButton;
}

static void FillRoundedRect(HDC hdc, const RECT& rect, COLORREF fill_color, COLORREF border_color, int radius)
{
	HPEN pen = CreatePen(PS_SOLID, 1, border_color);
	HBRUSH brush = CreateSolidBrush(fill_color);
	HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
	HBRUSH old_brush = static_cast<HBRUSH>(SelectObject(hdc, brush));
	RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
	SelectObject(hdc, old_brush);
	SelectObject(hdc, old_pen);
	DeleteObject(brush);
	DeleteObject(pen);
}

static void DrawInstallerCard(HDC hdc, const RECT& rect)
{
	FillRoundedRect(hdc, rect, RGB(255, 255, 255), RGB(220, 227, 235), 18);
}

static RECT MakeRect(int left, int top, int width, int height)
{
	RECT rect = { left, top, left + width, top + height };
	return rect;
}

static void PaintInstallerWindow(HWND window)
{
	PAINTSTRUCT paint = {};
	HDC hdc = BeginPaint(window, &paint);
	if (hdc == NULL) {
		return;
	}

	RECT client = {};
	GetClientRect(window, &client);
	FillRect(hdc, &client, g_app->BackgroundBrush);

	const int margin = 20;
	const int content_width = (client.right - client.left) - margin * 2;
	RECT hero_card = MakeRect(margin, 18, content_width, 92);
	RECT path_card = MakeRect(margin, 120, content_width, 86);
	RECT action_card = MakeRect(margin, 232, content_width, 62);
	RECT log_card = MakeRect(margin, 304, content_width, (client.bottom - 324) > 120 ? (client.bottom - 324) : 120);

	DrawInstallerCard(hdc, hero_card);
	DrawInstallerCard(hdc, path_card);
	DrawInstallerCard(hdc, action_card);
	DrawInstallerCard(hdc, log_card);

	RECT path_shell = MakeRect(30, 150, 490, 50);
	RECT log_shell = MakeRect(30, 346, content_width - 40, log_card.bottom - 346 - 10);
	FillRoundedRect(hdc, path_shell, RGB(248, 250, 252), RGB(214, 223, 233), 16);
	FillRoundedRect(hdc, log_shell, RGB(248, 250, 252), RGB(214, 223, 233), 16);

	EndPaint(window, &paint);
}

static void DrawButtonFrame(DRAWITEMSTRUCT* draw)
{
	const bool primary = IsPrimaryButton(draw->hwndItem);
	const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
	const bool pressed = (draw->itemState & ODS_SELECTED) != 0;

	COLORREF fill = RGB(255, 255, 255);
	COLORREF border = RGB(197, 206, 216);
	COLORREF text = RGB(36, 42, 49);

	if (primary) {
		fill = pressed ? RGB(29, 94, 214) : RGB(37, 99, 235);
		border = fill;
		text = RGB(255, 255, 255);
	}
	if (!primary && pressed) {
		fill = RGB(243, 247, 251);
		border = RGB(166, 177, 191);
	}
	if (disabled) {
		fill = primary ? RGB(162, 193, 245) : RGB(244, 246, 248);
		border = RGB(214, 220, 227);
		text = RGB(142, 149, 158);
	}

	RECT rect = draw->rcItem;
	FillRoundedRect(draw->hDC, rect, fill, border, 14);

	SetBkMode(draw->hDC, TRANSPARENT);
	SetTextColor(draw->hDC, text);
	HFONT old_font = static_cast<HFONT>(SelectObject(draw->hDC, g_app->UiFont));
	RECT text_rect = rect;
	if (pressed) {
		OffsetRect(&text_rect, 0, 1);
	}
	DrawTextW(draw->hDC, L"", 0, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

	wchar_t text_buffer[128] = {};
	GetWindowTextW(draw->hwndItem, text_buffer, 128);
	DrawTextW(draw->hDC, text_buffer, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	SelectObject(draw->hDC, old_font);
}

static void InitializeVisuals()
{
	if (g_app == NULL) return;

	g_app->HeaderFont = CreateUiFont(-28, FW_SEMIBOLD, false, L"Segoe UI");
	g_app->HintFont = CreateUiFont(-16, FW_NORMAL, false, L"Segoe UI");
	g_app->MonoFont = CreateUiFont(-16, FW_NORMAL, false, L"Segoe UI");
	g_app->BackgroundBrush = CreateSolidBrush(RGB(245, 247, 250));
	g_app->CardBrush = CreateSolidBrush(RGB(255, 255, 255));
	g_app->InputBrush = CreateSolidBrush(RGB(255, 255, 255));
	g_app->AccentBrush = CreateSolidBrush(RGB(37, 99, 235));
	g_app->AppIconLarge = static_cast<HICON>(LoadImageW(g_app->Instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR));
	g_app->AppIconSmall = static_cast<HICON>(LoadImageW(g_app->Instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 20, 20, LR_DEFAULTCOLOR));

	if (g_app->HeaderFont == NULL) g_app->HeaderFont = g_app->UiFont;
	if (g_app->HintFont == NULL) g_app->HintFont = g_app->UiFont;
	if (g_app->MonoFont == NULL) g_app->MonoFont = g_app->UiFont;
}

static bool CreateChildControls()
{
	if (g_app == NULL) return false;

	g_app->BrandIcon = CreateAppControl(WS_CHILD | WS_VISIBLE | SS_ICON, 0, L"STATIC", L"", 28, 26, 56, 56, IDC_BRAND_ICON);
	if (g_app->BrandIcon != NULL) {
		SendMessageW(g_app->BrandIcon, STM_SETICON, reinterpret_cast<WPARAM>(g_app->AppIconLarge), 0);
	}

	g_app->HeaderLabel = CreateAppControl(WS_CHILD | WS_VISIBLE, 0, L"STATIC", L"InsetFace setup", 102, 28, 420, 32, IDC_HEADER_TITLE);
	if (g_app->HeaderLabel != NULL) {
		SendMessageW(g_app->HeaderLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_app->HeaderFont), TRUE);
	}

	g_app->HintLabel = CreateAppControl(WS_CHILD | WS_VISIBLE, 0, L"STATIC", L"Auto-detect a Metasequoia install, or point the installer at Metaseq.exe manually.", 102, 64, 590, 24, IDC_HEADER_HINT);
	if (g_app->HintLabel != NULL) {
		SendMessageW(g_app->HintLabel, WM_SETFONT, reinterpret_cast<WPARAM>(g_app->HintFont), TRUE);
	}

	CreateAppControl(WS_CHILD | WS_VISIBLE, 0, L"STATIC", L"Metasequoia root", 36, 132, 180, 20, 0);
	g_app->PathEdit = CreateAppControl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, L"EDIT", L"", 36, 160, 472, 34, IDC_ROOT_PATH);
	if (g_app->PathEdit != NULL) {
		SendMessageW(g_app->PathEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
	}
	g_app->BrowseButton = CreateAppControl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, L"BUTTON", L"Browse...", 520, 160, 116, 34, IDC_BROWSE);

	g_app->InstallButton = CreateAppControl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, L"BUTTON", L"Install / Update", 36, 244, 184, 40, IDC_INSTALL);
	g_app->RepairButton = CreateAppControl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, L"BUTTON", L"Repair", 232, 244, 128, 40, IDC_REPAIR);
	g_app->UninstallButton = CreateAppControl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, L"BUTTON", L"Uninstall", 372, 244, 128, 40, IDC_UNINSTALL);

	g_app->StatusLabel = CreateAppControl(WS_CHILD | WS_VISIBLE, 0, L"STATIC", L"Activity log", 36, 324, 160, 20, IDC_SECTION_STATUS);
	g_app->StatusEdit = CreateAppControl(
		WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
		0,
		L"EDIT",
		L"",
		36,
		352,
		604,
		180,
		IDC_STATUS);
	if (g_app->StatusEdit != NULL) {
		SendMessageW(g_app->StatusEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_app->MonoFont), TRUE);
		SendMessageW(g_app->StatusEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
	}

	return g_app->PathEdit != NULL &&
		g_app->BrowseButton != NULL &&
		g_app->InstallButton != NULL &&
		g_app->StatusEdit != NULL;
}

static LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	switch (message) {
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		if (g_app != NULL) {
			PaintInstallerWindow(window);
			return 0;
		}
		break;
	case WM_DRAWITEM:
		if (g_app != NULL) {
			DRAWITEMSTRUCT* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
			if (draw != NULL && draw->CtlType == ODT_BUTTON) {
				DrawButtonFrame(draw);
				return TRUE;
			}
		}
		break;
	case WM_CTLCOLOREDIT:
		if (g_app != NULL) {
			HDC hdc = reinterpret_cast<HDC>(wparam);
			SetBkMode(hdc, OPAQUE);
			SetBkColor(hdc, RGB(255, 255, 255));
			SetTextColor(hdc, RGB(35, 41, 49));
			return reinterpret_cast<LRESULT>(g_app->InputBrush);
		}
		break;
	case WM_CTLCOLORSTATIC:
		if (g_app != NULL) {
			HDC hdc = reinterpret_cast<HDC>(wparam);
			HWND control = reinterpret_cast<HWND>(lparam);
			if (control == g_app->StatusEdit) {
				SetBkMode(hdc, OPAQUE);
				SetBkColor(hdc, RGB(255, 255, 255));
				SetTextColor(hdc, RGB(35, 41, 49));
				return reinterpret_cast<LRESULT>(g_app->InputBrush);
			}

			SetBkMode(hdc, TRANSPARENT);
			if (control == g_app->HeaderLabel) {
				SetTextColor(hdc, RGB(23, 43, 77));
			}
			else if (control == g_app->HintLabel || control == g_app->StatusLabel) {
				SetTextColor(hdc, RGB(95, 103, 115));
			}
			else {
				SetTextColor(hdc, RGB(44, 52, 64));
			}
			return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
		}
		break;
	case WM_COMMAND:
		switch (LOWORD(wparam)) {
		case IDC_ROOT_PATH:
			if (HIWORD(wparam) == EN_CHANGE) {
				RefreshButtons();
			}
			return 0;
		case IDC_BROWSE:
			ChooseMetaseqExecutable(window);
			return 0;
		case IDC_INSTALL:
			RunAction(InstallAction::Install);
			return 0;
		case IDC_REPAIR:
			RunAction(InstallAction::Repair);
			return 0;
		case IDC_UNINSTALL:
			RunAction(InstallAction::Uninstall);
			return 0;
		default:
			break;
		}
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int show_command)
{
	INITCOMMONCONTROLSEX common_controls = {};
	common_controls.dwSize = sizeof(common_controls);
	common_controls.dwICC = ICC_STANDARD_CLASSES;
	InitCommonControlsEx(&common_controls);

	AppState app = {};
	app.Instance = instance;
	app.UiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
	g_app = &app;
	InitializeVisuals();

	WNDCLASSEXW window_class = {};
	window_class.cbSize = sizeof(window_class);
	window_class.lpfnWndProc = MainWindowProc;
	window_class.hInstance = instance;
	window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
	window_class.hbrBackground = app.BackgroundBrush != NULL ? app.BackgroundBrush : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	window_class.hIcon = app.AppIconLarge;
	window_class.hIconSm = app.AppIconSmall;
	window_class.lpszClassName = L"InsetFaceInstallerWindow";

	if (!RegisterClassExW(&window_class)) {
		return 1;
	}

	app.Window = CreateWindowExW(
		0,
		window_class.lpszClassName,
		kAppTitle,
		WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		700,
		570,
		NULL,
		NULL,
		instance,
		NULL);
	if (app.Window == NULL) {
		return 1;
	}

	if (!CreateChildControls()) {
		return 1;
	}

	std::wstring detected_root = AutoDetectMetasequoiaRoot();
	if (app.PathEdit != NULL && !detected_root.empty()) {
		SetWindowTextW(app.PathEdit, detected_root.c_str());
		AppendStatus(L"Auto-detected the Metasequoia install folder.");
	}
	else {
		AppendStatus(L"Auto-detection did not find Metasequoia. Paste the install folder path or browse to Metaseq.exe.");
	}

	RefreshButtons();
	ShowWindow(app.Window, show_command);
	UpdateWindow(app.Window);

	MSG message = {};
	while (GetMessageW(&message, NULL, 0, 0) > 0) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	if (app.HeaderFont != NULL && app.HeaderFont != app.UiFont) DeleteObject(app.HeaderFont);
	if (app.HintFont != NULL && app.HintFont != app.UiFont) DeleteObject(app.HintFont);
	if (app.MonoFont != NULL && app.MonoFont != app.UiFont) DeleteObject(app.MonoFont);
	if (app.AppIconLarge != NULL) DestroyIcon(app.AppIconLarge);
	if (app.AppIconSmall != NULL) DestroyIcon(app.AppIconSmall);
	if (app.BackgroundBrush != NULL) DeleteObject(app.BackgroundBrush);
	if (app.CardBrush != NULL) DeleteObject(app.CardBrush);
	if (app.InputBrush != NULL) DeleteObject(app.InputBrush);
	if (app.AccentBrush != NULL) DeleteObject(app.AccentBrush);

	return static_cast<int>(message.wParam);
}
