#include "Voicevox.h"

#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <utility>
#include <tlhelp32.h>

#pragma comment(lib, "winhttp.lib")

namespace Voicevox
{
	namespace
	{
		class InternetHandle
		{
		public:
			explicit InternetHandle(HINTERNET handle = nullptr) : handle_(handle) {}
			~InternetHandle() { if (handle_) WinHttpCloseHandle(handle_); }
			InternetHandle(const InternetHandle&) = delete;
			InternetHandle& operator=(const InternetHandle&) = delete;
			HINTERNET Get() const { return handle_; }

		private:
			HINTERNET handle_;
		};

		std::wstring GetEnvironmentPath(const wchar_t* name)
		{
			const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
			if (length == 0) return {};
			std::vector<wchar_t> value(length);
			const DWORD written = GetEnvironmentVariableW(name, value.data(), length);
			if (written == 0 || written >= length) return {};
			return std::wstring(value.data(), written);
		}

		std::filesystem::path FindEngineExecutable()
		{
			const std::wstring localAppData = GetEnvironmentPath(L"LOCALAPPDATA");
			if (!localAppData.empty())
			{
				const std::filesystem::path bundled = std::filesystem::path(localAppData) /
					L"Programs" / L"VOICEVOX" / L"vv-engine" / L"run.exe";
				if (std::filesystem::exists(bundled)) return bundled;
			}

			const std::wstring roaming = GetEnvironmentPath(L"APPDATA");
			if (roaming.empty()) return {};
			const std::filesystem::path engines = std::filesystem::path(roaming) / L"VOICEVOX" / L"vvpp-engines";
			std::error_code error;
			for (std::filesystem::directory_iterator it(engines, error), end; !error && it != end; it.increment(error))
			{
				if (!it->is_directory(error)) continue;
				const std::wstring folderName = it->path().filename().native();
				if (folderName.rfind(L"VOICEVOX_Engine+", 0) != 0) continue;
				const std::filesystem::path executable = it->path() / L"run.exe";
				if (std::filesystem::exists(executable)) return executable;
			}
			return {};
		}

		bool IsEngineProcessRunning(const std::filesystem::path& executable)
		{
			HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
			if (snapshot == INVALID_HANDLE_VALUE) return false;
			PROCESSENTRY32W entry = {};
			entry.dwSize = sizeof(entry);
			bool running = false;
			if (Process32FirstW(snapshot, &entry))
			{
				do
				{
					if (_wcsicmp(entry.szExeFile, L"run.exe") != 0) continue;
					HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
					if (process == nullptr) continue;
					std::vector<wchar_t> processPath(32768);
					DWORD pathLength = static_cast<DWORD>(processPath.size());
					if (QueryFullProcessImageNameW(process, 0, processPath.data(), &pathLength) &&
						_wcsicmp(std::filesystem::path(processPath.data()).c_str(), executable.c_str()) == 0)
						running = true;
					CloseHandle(process);
					if (running) break;
				} while (Process32NextW(snapshot, &entry));
			}
			CloseHandle(snapshot);
			return running;
		}

		bool SendRequest(const wchar_t* method, const std::wstring& path,
			const std::string& requestBody, std::vector<BYTE>& responseBody, DWORD timeout = 15000)
		{
			InternetHandle session(WinHttpOpen(L"DesktopMascot/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
				WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
			if (!session.Get()) return false;
			WinHttpSetTimeouts(session.Get(), 500, 500, 500, timeout);

			InternetHandle connection(WinHttpConnect(session.Get(), L"127.0.0.1", 50021, 0));
			if (!connection.Get()) return false;
			InternetHandle request(WinHttpOpenRequest(connection.Get(), method, path.c_str(), nullptr,
				WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
			if (!request.Get()) return false;

			const wchar_t* headers = L"Content-Type: application/json; charset=utf-8\r\n";
			const DWORD bodyLength = static_cast<DWORD>(requestBody.size());
			if (!WinHttpSendRequest(request.Get(), headers, static_cast<DWORD>(-1),
				requestBody.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(requestBody.data()),
				bodyLength, bodyLength, 0))
				return false;
			if (!WinHttpReceiveResponse(request.Get(), nullptr)) return false;

			DWORD statusCode = 0;
			DWORD statusSize = sizeof(statusCode);
			if (!WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX) || statusCode < 200 || statusCode >= 300)
				return false;

			for (;;)
			{
				DWORD available = 0;
				if (!WinHttpQueryDataAvailable(request.Get(), &available)) return false;
				if (available == 0) break;
				const size_t oldSize = responseBody.size();
				responseBody.resize(oldSize + available);
				DWORD read = 0;
				if (!WinHttpReadData(request.Get(), responseBody.data() + oldSize, available, &read)) return false;
				responseBody.resize(oldSize + read);
			}
			return true;
		}

		std::string ResponseText(const std::vector<BYTE>& data)
		{
			return std::string(data.begin(), data.end());
		}

		bool ReadJsonString(const std::string& json, const std::string& key, size_t from, size_t end, std::string& value)
		{
			const size_t keyPosition = json.find("\"" + key + "\"", from);
			if (keyPosition == std::string::npos || keyPosition >= end) return false;
			const size_t colon = json.find(':', keyPosition + key.size() + 2);
			if (colon == std::string::npos || colon >= end) return false;
			const size_t quote = json.find('"', colon + 1);
			if (quote == std::string::npos || quote >= end) return false;
			value.clear();
			for (size_t i = quote + 1; i < end; ++i)
			{
				if (json[i] == '"') return true;
				if (json[i] == '\\' && i + 1 < end)
				{
					++i;
					if (json[i] == 'n') value.push_back('\n');
					else if (json[i] == 'r') value.push_back('\r');
					else if (json[i] == 't') value.push_back('\t');
					else value.push_back(json[i]);
				}
				else value.push_back(json[i]);
			}
			return false;
		}

		bool ReadJsonInteger(const std::string& json, const std::string& key, size_t from, size_t end, int& value)
		{
			const size_t keyPosition = json.find("\"" + key + "\"", from);
			if (keyPosition == std::string::npos || keyPosition >= end) return false;
			const size_t colon = json.find(':', keyPosition + key.size() + 2);
			if (colon == std::string::npos || colon >= end) return false;
			const char* start = json.c_str() + colon + 1;
			while (*start && std::isspace(static_cast<unsigned char>(*start))) ++start;
			char* parsedEnd = nullptr;
			const long parsed = std::strtol(start, &parsedEnd, 10);
			if (parsedEnd == start || parsed < 0 || parsed > INT_MAX) return false;
			value = static_cast<int>(parsed);
			return true;
		}

		std::vector<Speaker> ParseSpeakers(const std::string& json)
		{
			std::vector<Speaker> speakers;
			size_t position = 0;
			while ((position = json.find("\"styles\"", position)) != std::string::npos)
			{
				const size_t speakerStart = json.rfind('{', position);
				const size_t stylesStart = json.find('[', position);
				const size_t stylesEnd = json.find(']', stylesStart);
				if (speakerStart == std::string::npos || stylesStart == std::string::npos || stylesEnd == std::string::npos)
					break;
				std::string speakerName;
				if (!ReadJsonString(json, "name", speakerStart, position, speakerName))
				{
					position = stylesEnd + 1;
					continue;
				}

				size_t stylePosition = stylesStart + 1;
				while ((stylePosition = json.find('{', stylePosition)) != std::string::npos && stylePosition < stylesEnd)
				{
					const size_t styleEnd = json.find('}', stylePosition);
					if (styleEnd == std::string::npos || styleEnd > stylesEnd) break;
					std::string styleName;
					int id = 0;
					if (ReadJsonString(json, "name", stylePosition, styleEnd, styleName) &&
						ReadJsonInteger(json, "id", stylePosition, styleEnd, id))
						speakers.push_back({ id, speakerName + " / " + styleName });
					stylePosition = styleEnd + 1;
				}
				position = stylesEnd + 1;
			}
			return speakers;
		}

		bool ReplaceJsonNumber(std::string& json, const std::string& key, float value)
		{
			const size_t keyPosition = json.find("\"" + key + "\"");
			if (keyPosition == std::string::npos) return false;
			const size_t colon = json.find(':', keyPosition + key.size() + 2);
			if (colon == std::string::npos) return false;
			size_t start = colon + 1;
			while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;
			size_t end = start;
			while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) ||
				json[end] == '-' || json[end] == '+' || json[end] == '.' || json[end] == 'e' || json[end] == 'E')) ++end;
			if (start == end) return false;
			json.replace(start, end - start, std::to_string(value));
			return true;
		}
	}

	TaskResult FetchSpeakers()
	{
		TaskResult result;
		std::vector<BYTE> response;
		if (!SendRequest(L"GET", L"/speakers", {}, response))
		{
			result.error = "connection";
			return result;
		}
		result.speakers = ParseSpeakers(ResponseText(response));
		result.success = !result.speakers.empty();
		if (!result.success) result.error = "speaker_list";
		return result;
	}

	EngineStartResult EnsureEngineStarted()
	{
		std::vector<BYTE> versionResponse;
		if (SendRequest(L"GET", L"/version", {}, versionResponse, 500))
			return EngineStartResult::AlreadyRunning;
		const std::filesystem::path executable = FindEngineExecutable();
		if (executable.empty()) return EngineStartResult::NotFound;
		if (IsEngineProcessRunning(executable)) return EngineStartResult::AlreadyRunning;

		std::wstring command = L"\"" + executable.wstring() + L"\"";
		std::vector<wchar_t> mutableCommand(command.begin(), command.end());
		mutableCommand.push_back(L'\0');
		STARTUPINFOW startup = {};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESHOWWINDOW;
		startup.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION process = {};
		if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process))
			return EngineStartResult::Failed;
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		return EngineStartResult::Started;
	}

	TaskResult SynthesizeTest(int speakerId, float speed, float pitch, float volume)
	{
		TaskResult result;
		constexpr wchar_t queryPath[] = L"/audio_query?text=%E3%83%86%E3%82%B9%E3%83%88%E5%86%8D%E7%94%9F%E3%81%A0%E3%82%88%E3%80%82&speaker=";
		const std::wstring fullQueryPath = std::wstring(queryPath) + std::to_wstring(speakerId);
		std::vector<BYTE> queryResponse;
		if (!SendRequest(L"POST", fullQueryPath, {}, queryResponse))
		{
			result.error = "audio_query";
			return result;
		}
		std::string query = ResponseText(queryResponse);
		if (!ReplaceJsonNumber(query, "speedScale", speed) || !ReplaceJsonNumber(query, "pitchScale", pitch) ||
			!ReplaceJsonNumber(query, "volumeScale", volume))
		{
			result.error = "audio_query_format";
			return result;
		}

		const std::wstring synthesisPath = L"/synthesis?speaker=" + std::to_wstring(speakerId);
		std::vector<BYTE> wavResponse;
		if (!SendRequest(L"POST", synthesisPath, query, wavResponse))
		{
			result.error = "synthesis";
			return result;
		}
		result.wavData = std::move(wavResponse);
		result.success = result.wavData.size() > 44;
		if (!result.success) result.error = "empty_audio";
		return result;
	}
}
