#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "Texture.h"
#include "Audio.h"
#include "Voicevox.h"

#include <Windows.h>
#include <windowsx.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <future>
#include <fstream>
#include <filesystem>
#include <memory>
#include <stdlib.h>
#include <assert.h>
#include <string>
#include <cstdlib>
#include <time.h>
#include <vector>

#include "global.h"

#include "resource.h"

#pragma comment(lib,"Winmm.lib")

//定数宣言
const char* WIN_CLASS_NAME = "SampleGame";	//ウィンドウクラス名
constexpr UINT MENU_TOGGLE_TOPMOST = 1001;
constexpr UINT MENU_EXIT = 1002;
constexpr UINT MENU_SETTINGS = 1003;
bool isAlwaysOnTop = true;
bool startWithWindows = false;
bool autoStartVoicevox = true;
int animationFps = 12;
float voiceVolume = 1.0f;
float voiceSpeed = 1.0f;
float voicePitch = 0.0f;
int selectedVoiceId = 1;
std::vector<Voicevox::Speaker> voiceSpeakers;
std::future<Voicevox::TaskResult> voiceTask;
enum class VoiceTaskKind { None, FetchSpeakers, SynthesizeTest };
VoiceTaskKind voiceTaskKind = VoiceTaskKind::None;
std::string voiceStatus = "未接続";
HWND settingsHWnd = nullptr;
HWND mascotHWnd = nullptr;
IDXGISwapChain1* settingsSwapChain = nullptr;
ID3D11RenderTargetView* settingsRenderTarget = nullptr;
ImGuiContext* settingsImGuiContext = nullptr;
ImGuiContext* mascotImGuiContext = nullptr;
std::string characterConfigPath = "..\\_mods\\Character\\MyMascot\\character.json";
std::string activeCharacterFolder = "MyMascot";
std::string loadedCharacterFolder = "MyMascot";
std::string pendingCharacterFolder;

struct CharacterInfo
{
	std::string folder;
	std::string displayName;
};

std::vector<CharacterInfo> availableCharacters;


//プロトタイプ宣言
HWND InitApp(HINSTANCE hInstance, int screenWidth, int screenHeight, int nCmdShow);
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
bool OpenSettingsWindow(HWND mascotWindow);
void RenderSettingsWindow(HWND mascotWindow);
void ReleaseSettingsWindow();

void AddJapaneseFont()
{
	char windowsDirectory[MAX_PATH] = {};
	GetWindowsDirectoryA(windowsDirectory, MAX_PATH);
	const char* fontNames[] = { "\\Fonts\\meiryo.ttc", "\\Fonts\\YuGothR.ttc", "\\Fonts\\msgothic.ttc" };
	ImGuiIO& io = ImGui::GetIO();
	for (const char* fontName : fontNames)
	{
		const std::string fontPath = std::string(windowsDirectory) + fontName;
		if (GetFileAttributesA(fontPath.c_str()) == INVALID_FILE_ATTRIBUTES)
			continue;
		ImFont* japaneseFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 18.0f, nullptr,
			io.Fonts->GetGlyphRangesJapanese());
		if (japaneseFont != nullptr)
		{
			io.FontDefault = japaneseFont;
			return;
		}
	}
}

bool ReadJsonStringValue(const std::string& json, const char* key, std::string& value)
{
	const size_t keyPosition = json.find(std::string("\"") + key + "\"");
	if (keyPosition == std::string::npos) return false;
	const size_t colon = json.find(':', keyPosition);
	const size_t quote = colon == std::string::npos ? std::string::npos : json.find('"', colon + 1);
	if (quote == std::string::npos) return false;
	const size_t endQuote = json.find('"', quote + 1);
	if (endQuote == std::string::npos) return false;
	value = json.substr(quote + 1, endQuote - quote - 1);
	return true;
}

std::string GetCharacterConfigPath(const std::string& folder)
{
	return "..\\_mods\\Character\\" + folder + "\\character.json";
}

void FindCharacters()
{
	availableCharacters.clear();
	const std::filesystem::path characterRoot("..\\_mods\\Character");
	std::error_code error;
	for (std::filesystem::directory_iterator it(characterRoot, error), end; !error && it != end; it.increment(error))
	{
		if (!it->is_directory(error)) continue;
		const std::string folder = it->path().filename().string();
		std::ifstream file(GetCharacterConfigPath(folder), std::ios::binary);
		if (!file) continue;
		const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		std::string displayName;
		if (!ReadJsonStringValue(json, "displayName", displayName)) displayName = folder;
		availableCharacters.push_back({ folder, displayName });
	}
	std::sort(availableCharacters.begin(), availableCharacters.end(),
		[](const CharacterInfo& left, const CharacterInfo& right) { return left.displayName < right.displayName; });
}

bool LoadIdleFrames(const std::string& folder, std::vector<std::unique_ptr<Texture>>& frames)
{
	std::vector<std::unique_ptr<Texture>> loadedFrames;
	for (int i = 0; i < 12; ++i)
	{
		std::string framePath = "..\\_mods\\Character\\" + folder + "\\Animation\\Idle\\idle_";
		framePath += i < 10 ? "00" : "0";
		framePath += std::to_string(i) + ".png";
		std::unique_ptr<Texture> frame = std::make_unique<Texture>();
		if (FAILED(frame->Load(framePath, true))) return false;
		loadedFrames.push_back(std::move(frame));
	}
	frames = std::move(loadedFrames);
	return true;
}

bool ReadCharacterVoiceValue(const std::string& json, const char* key, float& value)
{
	const size_t voiceStart = json.find("\"voice\"");
	if (voiceStart == std::string::npos) return false;
	const size_t voiceEnd = json.find('}', voiceStart);
	const size_t keyPosition = json.find(std::string("\"") + key + "\"", voiceStart);
	if (keyPosition == std::string::npos || keyPosition > voiceEnd) return false;
	const size_t colon = json.find(':', keyPosition);
	if (colon == std::string::npos || colon > voiceEnd) return false;
	char* end = nullptr;
	const float parsed = std::strtof(json.c_str() + colon + 1, &end);
	if (end == json.c_str() + colon + 1) return false;
	value = parsed;
	return true;
}

bool ReadCharacterVoiceValue(const std::string& json, const char* key, int& value)
{
	float parsed = 0.0f;
	if (!ReadCharacterVoiceValue(json, key, parsed)) return false;
	value = static_cast<int>(parsed);
	return true;
}

bool WriteCharacterVoiceValue(std::string& json, const char* key, const std::string& value)
{
	const size_t voiceStart = json.find("\"voice\"");
	if (voiceStart == std::string::npos) return false;
	const size_t voiceEnd = json.find('}', voiceStart);
	const size_t keyPosition = json.find(std::string("\"") + key + "\"", voiceStart);
	if (keyPosition == std::string::npos || keyPosition > voiceEnd) return false;
	const size_t colon = json.find(':', keyPosition);
	if (colon == std::string::npos || colon > voiceEnd) return false;
	size_t start = colon + 1;
	while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start]))) ++start;
	size_t end = start;
	while (end < json.size() && (std::isdigit(static_cast<unsigned char>(json[end])) ||
		json[end] == '-' || json[end] == '+' || json[end] == '.' || json[end] == 'e' || json[end] == 'E')) ++end;
	if (start == end || end > voiceEnd) return false;
	json.replace(start, end - start, value);
	return true;
}

void LoadCharacterVoiceDefaults()
{
	std::ifstream file(characterConfigPath, std::ios::binary);
	if (!file) return;
	const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	ReadCharacterVoiceValue(json, "speakerId", selectedVoiceId);
	ReadCharacterVoiceValue(json, "volume", voiceVolume);
	ReadCharacterVoiceValue(json, "speed", voiceSpeed);
	ReadCharacterVoiceValue(json, "pitch", voicePitch);
	voiceVolume = (std::clamp)(voiceVolume, 0.0f, 2.0f);
	voiceSpeed = (std::clamp)(voiceSpeed, 0.5f, 2.0f);
	voicePitch = (std::clamp)(voicePitch, -0.15f, 0.15f);
}

void SaveCharacterVoiceDefaults()
{
	std::ifstream input(characterConfigPath, std::ios::binary);
	if (!input) return;
	std::string json((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	input.close();
	if (!WriteCharacterVoiceValue(json, "speakerId", std::to_string(selectedVoiceId)) ||
		!WriteCharacterVoiceValue(json, "volume", std::to_string(voiceVolume)) ||
		!WriteCharacterVoiceValue(json, "speed", std::to_string(voiceSpeed)) ||
		!WriteCharacterVoiceValue(json, "pitch", std::to_string(voicePitch)))
		return;
	std::ofstream output(characterConfigPath, std::ios::binary | std::ios::trunc);
	if (output) output.write(json.data(), static_cast<std::streamsize>(json.size()));
}

bool SetRunAtStartup(bool enabled)
{
	HKEY runKey = nullptr;
	const LONG openResult = RegCreateKeyExW(HKEY_CURRENT_USER,
		L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &runKey, nullptr);
	if (openResult != ERROR_SUCCESS)
		return false;

	LONG result = ERROR_SUCCESS;
	if (enabled)
	{
		std::vector<wchar_t> executablePath(32768);
		const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
		if (pathLength == 0 || pathLength >= executablePath.size())
			result = ERROR_INSUFFICIENT_BUFFER;
		else
		{
			const std::wstring command = L"\"" + std::wstring(executablePath.data(), pathLength) + L"\"";
			result = RegSetValueExW(runKey, L"DesktopMascot", 0, REG_SZ,
				reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
		}
	}
	else
	{
		result = RegDeleteValueW(runKey, L"DesktopMascot");
		if (result == ERROR_FILE_NOT_FOUND)
			result = ERROR_SUCCESS;
	}
	RegCloseKey(runKey);
	return result == ERROR_SUCCESS;
}

void EnsureVoicevoxEngineStarted()
{
	switch (Voicevox::EnsureEngineStarted())
	{
	case Voicevox::EngineStartResult::AlreadyRunning:
		voiceStatus = "VOICEVOX ENGINEは起動済みだよ。";
		break;
	case Voicevox::EngineStartResult::Started:
		voiceStatus = "VOICEVOX ENGINEを起動してるよ...";
		break;
	case Voicevox::EngineStartResult::NotFound:
		voiceStatus = "VOICEVOX ENGINEが見つからなかったよ。";
		break;
	case Voicevox::EngineStartResult::Failed:
		voiceStatus = "VOICEVOX ENGINEを起動できなかったよ。";
		break;
	}
}

bool CreateSettingsRenderTarget()
{
	if (settingsSwapChain == nullptr)
		return false;
	ID3D11Texture2D* backBuffer = nullptr;
	HRESULT hr = settingsSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
	if (SUCCEEDED(hr))
		hr = Direct3D::pDevice_->CreateRenderTargetView(backBuffer, nullptr, &settingsRenderTarget);
	SAFE_RELEASE(backBuffer);
	return SUCCEEDED(hr);
}

void ResizeSettingsWindow()
{
	if (settingsSwapChain == nullptr || settingsHWnd == nullptr || IsIconic(settingsHWnd))
		return;
	RECT client = {};
	GetClientRect(settingsHWnd, &client);
	const UINT width = static_cast<UINT>(client.right - client.left);
	const UINT height = static_cast<UINT>(client.bottom - client.top);
	if (width == 0 || height == 0)
		return;
	Direct3D::pContext_->OMSetRenderTargets(0, nullptr, nullptr);
	SAFE_RELEASE(settingsRenderTarget);
	if (SUCCEEDED(settingsSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)))
		CreateSettingsRenderTarget();
}

bool OpenSettingsWindow(HWND mascotWindow)
{
	if (settingsHWnd != nullptr)
	{
		SetWindowPos(mascotWindow, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		ShowWindow(settingsHWnd, SW_RESTORE);
		SetForegroundWindow(settingsHWnd);
		return true;
	}

	HINSTANCE instance = GetModuleHandle(nullptr);
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.hInstance = instance;
	wc.lpszClassName = L"DesktopMascotSettings";
	wc.lpfnWndProc = SettingsWndProc;
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
	if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return false;

	settingsHWnd = CreateWindowExW(0, wc.lpszClassName, L"DesktopMascot 設定", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 580, 610, nullptr, nullptr, instance, nullptr);
	if (settingsHWnd == nullptr)
		return false;

	IDXGIDevice* dxgiDevice = nullptr;
	IDXGIAdapter* adapter = nullptr;
	IDXGIFactory2* factory = nullptr;
	HRESULT hr = Direct3D::pDevice_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
	if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
	if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
	DXGI_SWAP_CHAIN_DESC1 desc = {};
	desc.Width = 0;
	desc.Height = 0;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount = 2;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	if (SUCCEEDED(hr)) hr = factory->CreateSwapChainForHwnd(Direct3D::pDevice_, settingsHWnd, &desc, nullptr, nullptr, &settingsSwapChain);
	SAFE_RELEASE(factory);
	SAFE_RELEASE(adapter);
	SAFE_RELEASE(dxgiDevice);
	if (SUCCEEDED(hr) && !CreateSettingsRenderTarget())
		hr = E_FAIL;
	if (FAILED(hr))
	{
		ReleaseSettingsWindow();
		return false;
	}

	settingsImGuiContext = ImGui::CreateContext();
	ImGui::SetCurrentContext(settingsImGuiContext);
	ImGui::StyleColorsDark();
	AddJapaneseFont();
	ImGui_ImplWin32_Init(settingsHWnd);
	ImGui_ImplDX11_Init(Direct3D::pDevice_, Direct3D::pContext_);
	ImGui::SetCurrentContext(mascotImGuiContext);
	SetWindowPos(mascotWindow, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	ShowWindow(settingsHWnd, SW_SHOW);
	SetForegroundWindow(settingsHWnd);
	return true;
}

void RenderSettingsWindow(HWND mascotWindow)
{
	if (settingsHWnd == nullptr || !IsWindowVisible(settingsHWnd) || settingsImGuiContext == nullptr)
		return;
	ImGui::SetCurrentContext(settingsImGuiContext);
	if (voiceTask.valid() && voiceTask.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
	{
		Voicevox::TaskResult result = voiceTask.get();
		if (voiceTaskKind == VoiceTaskKind::FetchSpeakers)
		{
			if (result.success)
			{
				voiceSpeakers = std::move(result.speakers);
				const auto selected = std::find_if(voiceSpeakers.begin(), voiceSpeakers.end(),
					[](const Voicevox::Speaker& speaker) { return speaker.id == selectedVoiceId; });
				if (selected == voiceSpeakers.end())
				{
					selectedVoiceId = voiceSpeakers.front().id;
					SaveCharacterVoiceDefaults();
				}
				voiceStatus = "話者一覧を読み込んだよ。";
			}
			else
			{
				voiceStatus = "VOICEVOX ENGINEに接続できなかったよ。起動を確認してね。";
			}
		}
		else if (voiceTaskKind == VoiceTaskKind::SynthesizeTest)
		{
			if (result.success && Audio::PlayMemoryWav(result.wavData))
				voiceStatus = "テスト音声を再生してるよ。";
			else
				voiceStatus = "音声を作れなかったよ。VOICEVOX ENGINEを確認してね。";
		}
		voiceTaskKind = VoiceTaskKind::None;
	}
	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize, ImGuiCond_Always);
	ImGui::Begin("設定", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
	const CharacterInfo* activeCharacter = nullptr;
	for (const CharacterInfo& character : availableCharacters)
		if (character.folder == activeCharacterFolder) activeCharacter = &character;
	const char* activeCharacterName = activeCharacter ? activeCharacter->displayName.c_str() : activeCharacterFolder.c_str();
	if (ImGui::BeginCombo("キャラクター", activeCharacterName))
	{
		for (const CharacterInfo& character : availableCharacters)
		{
			const bool isSelected = character.folder == activeCharacterFolder;
			if (ImGui::Selectable(character.displayName.c_str(), isSelected) && !isSelected)
			{
				SaveCharacterVoiceDefaults();
				activeCharacterFolder = character.folder;
				characterConfigPath = GetCharacterConfigPath(activeCharacterFolder);
				LoadCharacterVoiceDefaults();
				pendingCharacterFolder = activeCharacterFolder;
				WritePrivateProfileString("CHARACTER", "Folder", activeCharacterFolder.c_str(), ".\\setup.ini");
				voiceStatus = "キャラクターを切り替えてるよ...";
			}
			if (isSelected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	bool startAtStartup = startWithWindows;
	if (ImGui::Checkbox("PC起動時に起動", &startAtStartup))
	{
		if (SetRunAtStartup(startAtStartup))
		{
			startWithWindows = startAtStartup;
			WritePrivateProfileString("WINDOW", "StartWithWindows", startWithWindows ? "1" : "0", ".\\setup.ini");
		}
		else
		{
			MessageBoxW(settingsHWnd, L"スタートアップ設定を保存できませんでした。", L"設定エラー", MB_OK | MB_ICONERROR);
		}
	}
	bool startVoicevox = autoStartVoicevox;
	if (ImGui::Checkbox("VOICEVOX ENGINEを自動起動", &startVoicevox))
	{
		autoStartVoicevox = startVoicevox;
		WritePrivateProfileString("VOICE", "AutoStartEngine", autoStartVoicevox ? "1" : "0", ".\\setup.ini");
		if (autoStartVoicevox) EnsureVoicevoxEngineStarted();
		else voiceStatus = "自動起動をオフにしたよ。必要なときはVOICEVOXを起動してね。";
	}
	bool topmost = isAlwaysOnTop;
	if (ImGui::Checkbox("常に最前面に表示", &topmost))
	{
		isAlwaysOnTop = topmost;
		WritePrivateProfileString("WINDOW", "AlwaysOnTop", isAlwaysOnTop ? "1" : "0", ".\\setup.ini");
	}
	if (ImGui::SliderInt("待機アニメの速さ (fps)", &animationFps, 1, 30))
	{
		char value[16];
		wsprintfA(value, "%d", animationFps);
		WritePrivateProfileString("ANIMATION", "IdleFps", value, ".\\setup.ini");
	}
	ImGui::Separator();
	ImGui::Text("VOICEVOX");
	if (!voiceTask.valid() && ImGui::Button("話者一覧を取得"))
	{
		voiceStatus = "VOICEVOXに接続してるよ...";
		voiceTaskKind = VoiceTaskKind::FetchSpeakers;
		voiceTask = std::async(std::launch::async, Voicevox::FetchSpeakers);
	}
	if (!voiceSpeakers.empty())
	{
		auto selected = std::find_if(voiceSpeakers.begin(), voiceSpeakers.end(),
			[](const Voicevox::Speaker& speaker) { return speaker.id == selectedVoiceId; });
		if (selected == voiceSpeakers.end()) selected = voiceSpeakers.begin();
		if (ImGui::BeginCombo("話者", selected->label.c_str()))
		{
			for (const Voicevox::Speaker& speaker : voiceSpeakers)
			{
				const bool isSelected = speaker.id == selectedVoiceId;
				if (ImGui::Selectable(speaker.label.c_str(), isSelected))
				{
					selectedVoiceId = speaker.id;
					char value[16];
					wsprintfA(value, "%d", selectedVoiceId);
					WritePrivateProfileString("VOICE", "SpeakerId", value, ".\\setup.ini");
					SaveCharacterVoiceDefaults();
				}
				if (isSelected) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (ImGui::SliderFloat("音量", &voiceVolume, 0.0f, 2.0f, "%.2f"))
		{
			char value[16];
			wsprintfA(value, "%d", static_cast<int>(voiceVolume * 1000.0f));
			WritePrivateProfileString("VOICE", "Volume", value, ".\\setup.ini");
			SaveCharacterVoiceDefaults();
		}
		if (ImGui::SliderFloat("話速", &voiceSpeed, 0.5f, 2.0f, "%.2f"))
		{
			char value[16];
			wsprintfA(value, "%d", static_cast<int>(voiceSpeed * 1000.0f));
			WritePrivateProfileString("VOICE", "Speed", value, ".\\setup.ini");
			SaveCharacterVoiceDefaults();
		}
		if (ImGui::SliderFloat("音高", &voicePitch, -0.15f, 0.15f, "%.2f"))
		{
			char value[16];
			wsprintfA(value, "%d", static_cast<int>(voicePitch * 1000.0f));
			WritePrivateProfileString("VOICE", "Pitch", value, ".\\setup.ini");
			SaveCharacterVoiceDefaults();
		}
		if (!voiceTask.valid() && ImGui::Button("テスト再生"))
		{
			voiceStatus = "テスト音声を作ってるよ...";
			voiceTaskKind = VoiceTaskKind::SynthesizeTest;
			const int speakerId = selectedVoiceId;
			const float speed = voiceSpeed;
			const float pitch = voicePitch;
			const float volume = voiceVolume;
			voiceTask = std::async(std::launch::async, [speakerId, speed, pitch, volume]()
				{ return Voicevox::SynthesizeTest(speakerId, speed, pitch, volume); });
		}
	}
	ImGui::TextWrapped("接続状態: %s", voiceStatus.c_str());
	if (ImGui::Button("キャラの位置を初期位置に戻す"))
	{
		MONITORINFO monitorInfo = {};
		monitorInfo.cbSize = sizeof(monitorInfo);
		HMONITOR monitor = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
		if (GetMonitorInfo(monitor, &monitorInfo))
		{
			RECT windowRect = {};
			GetWindowRect(mascotWindow, &windowRect);
			SetWindowPos(mascotWindow, nullptr, monitorInfo.rcWork.right - (windowRect.right - windowRect.left),
				monitorInfo.rcWork.bottom - (windowRect.bottom - windowRect.top), 0, 0,
				SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}
	ImGui::Text("最前面の変更は、この画面を閉じると反映されるよ。");
	ImGui::End();
	ImGui::Render();
	const float clearColor[4] = { 0.94f, 0.94f, 0.94f, 1.0f };
	Direct3D::pContext_->OMSetRenderTargets(1, &settingsRenderTarget, nullptr);
	Direct3D::pContext_->ClearRenderTargetView(settingsRenderTarget, clearColor);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	settingsSwapChain->Present(1, 0);
	ImGui::SetCurrentContext(mascotImGuiContext);
}

void ReleaseSettingsWindow()
{
	if (settingsImGuiContext != nullptr)
	{
		ImGui::SetCurrentContext(settingsImGuiContext);
		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext(settingsImGuiContext);
		settingsImGuiContext = nullptr;
		ImGui::SetCurrentContext(mascotImGuiContext);
	}
	SAFE_RELEASE(settingsRenderTarget);
	SAFE_RELEASE(settingsSwapChain);
	if (settingsHWnd != nullptr)
	{
		DestroyWindow(settingsHWnd);
		settingsHWnd = nullptr;
	}
}

void UpdateAlphaWindowRegion(HWND hWnd, Texture& texture, float scale, const ImVec2& imagePosition)
{
	const XMFLOAT3 size = texture.GetSize();
	const int width = static_cast<int>(size.x);
	const int height = static_cast<int>(size.y);
	const std::vector<BYTE>& alpha = texture.GetAlphaMask();
	HRGN windowRegion = CreateRectRgn(0, 0, 0, 0);
	constexpr int sampleStep = 8;

	for (int y = 0; y < height; y += sampleStep)
	{
		int runStart = -1;
		for (int x = 0; x <= width; x += sampleStep)
		{
			bool visible = false;
			if (x < width)
			{
				const int sampleX = (std::min)(x + sampleStep / 2, width - 1);
				const int sampleY = (std::min)(y + sampleStep / 2, height - 1);
				visible = alpha[static_cast<size_t>(sampleY) * width + sampleX] > 12;
			}

			if (visible && runStart < 0)
				runStart = x;
			else if (!visible && runStart >= 0)
			{
				const int left = static_cast<int>(std::floor(imagePosition.x + runStart * scale));
				const int right = static_cast<int>(std::ceil(imagePosition.x + x * scale));
				const int top = static_cast<int>(std::floor(imagePosition.y + y * scale));
				const int bottom = static_cast<int>(std::ceil(imagePosition.y + (std::min)(y + sampleStep, height) * scale));
				HRGN runRegion = CreateRectRgn(left, top, right, bottom);
				CombineRgn(windowRegion, windowRegion, runRegion, RGN_OR);
				DeleteObject(runRegion);
				runStart = -1;
			}
		}
	}

	if (!SetWindowRgn(hWnd, windowRegion, TRUE))
		DeleteObject(windowRegion);
}


// エントリーポイント
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	//#if defined(DEBUG) | defined(_DEBUG)
	//	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	//#endif

	srand((unsigned)time(NULL));
	SetCurrentDirectory("Assets");

	//初期化ファイル（setup.ini）から必要な情報を取得
	int screenWidth = GetPrivateProfileInt("SCREEN", "Width", 800, ".\\setup.ini");		//スクリーンの幅
	int screenHeight = GetPrivateProfileInt("SCREEN", "Height", 600, ".\\setup.ini");	//スクリーンの高さ
	int fpsLimit = GetPrivateProfileInt("GAME", "Fps", 60, ".\\setup.ini");				//FPS（画面更新速度）
	int isDrawFps = GetPrivateProfileInt("DEBUG", "ViewFps", 0, ".\\setup.ini");		//キャプションに現在のFPSを表示するかどうか
	isAlwaysOnTop = GetPrivateProfileInt("WINDOW", "AlwaysOnTop", 1, ".\\setup.ini") != 0;
	startWithWindows = GetPrivateProfileInt("WINDOW", "StartWithWindows", 0, ".\\setup.ini") != 0;
	animationFps = (std::clamp)(static_cast<int>(GetPrivateProfileInt("ANIMATION", "IdleFps", 12, ".\\setup.ini")), 1, 30);
	selectedVoiceId = static_cast<int>(GetPrivateProfileInt("VOICE", "SpeakerId", 1, ".\\setup.ini"));
	autoStartVoicevox = GetPrivateProfileInt("VOICE", "AutoStartEngine", 1, ".\\setup.ini") != 0;
	voiceVolume = (std::clamp)(static_cast<int>(GetPrivateProfileInt("VOICE", "Volume", 1000, ".\\setup.ini")) / 1000.0f, 0.0f, 2.0f);
	voiceSpeed = (std::clamp)(static_cast<int>(GetPrivateProfileInt("VOICE", "Speed", 1000, ".\\setup.ini")) / 1000.0f, 0.5f, 2.0f);
	voicePitch = (std::clamp)(static_cast<int>(GetPrivateProfileInt("VOICE", "Pitch", 0, ".\\setup.ini")) / 1000.0f, -0.15f, 0.15f);
	FindCharacters();
	char characterFolder[260] = {};
	GetPrivateProfileStringA("CHARACTER", "Folder", "MyMascot", characterFolder, sizeof(characterFolder), ".\\setup.ini");
	activeCharacterFolder = characterFolder;
	if (std::none_of(availableCharacters.begin(), availableCharacters.end(),
		[&](const CharacterInfo& character) { return character.folder == activeCharacterFolder; }) && !availableCharacters.empty())
		activeCharacterFolder = availableCharacters.front().folder;
	loadedCharacterFolder = activeCharacterFolder;
	characterConfigPath = GetCharacterConfigPath(activeCharacterFolder);
	LoadCharacterVoiceDefaults();




	//ウィンドウを作成
	HWND hWnd = InitApp(hInstance, screenWidth, screenHeight, nCmdShow);
	mascotHWnd = hWnd;
	SetWindowPos(hWnd, isAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

	//Direct3D準備
	HRESULT graphicsResult = Direct3D::Initialize(hWnd, screenWidth, screenHeight);
	if (FAILED(graphicsResult))
	{
		char errorMessage[256];
		wsprintfA(errorMessage, "透明な描画画面を作れませんでした。\n処理: %s\nHRESULT: 0x%08X",
			Direct3D::GetInitializationFailureStage(), static_cast<unsigned int>(graphicsResult));
		MessageBoxA(hWnd, errorMessage, "起動エラー", MB_OK | MB_ICONERROR);
		Direct3D::Release();
		DestroyWindow(hWnd);
		return -1;
	}
	Audio::Initialize();
	if (autoStartVoicevox) EnsureVoicevoxEngineStarted();

	//待機のコマを読み込んでおくね
	std::vector<std::unique_ptr<Texture>> idleFrames;
	if (!LoadIdleFrames(activeCharacterFolder, idleFrames))
	{
		MessageBoxW(hWnd, L"待機アニメーションの画像を読み込めませんでした。", L"画像読み込みエラー", MB_OK | MB_ICONERROR);
		Direct3D::Release();
		DestroyWindow(hWnd);
		return -1;
	}
	size_t lastRegionFrame = idleFrames.size();

	//ImGuiの準備
	IMGUI_CHECKVERSION();
	mascotImGuiContext = ImGui::CreateContext();
	ImGui::StyleColorsDark();

	ImGui_ImplWin32_Init(hWnd);
	ImGui_ImplDX11_Init(
		Direct3D::pDevice_,
		Direct3D::pContext_
	);


	//メッセージループ（何か起きるのを待つ）
	MSG msg;
	ZeroMemory(&msg, sizeof(msg));
	while (msg.message != WM_QUIT)
	{
		//メッセージあり（こっちが優先）
		if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		//メッセージなし（ここでゲームの処理）
		else
		{
			//時間計測
			timeBeginPeriod(1);	//時間計測の制度を上げる
			static int FPS = 0;								//画面更新回数のカウンタ
			static DWORD lastFpsResetTime = timeGetTime();	//最後にキャプションにFPSを表示した
			static DWORD lastUpdateTime = timeGetTime();	//最後に画面を更新した時間
			DWORD nowTime = timeGetTime();					//現在の時間

			//キャプションに現在のFPSを表示する
			if (isDrawFps)
			{
				//1秒（1000ミリ秒）経ったら
				if (nowTime - lastFpsResetTime > 1000)
				{
					//FPSの値を表示
					char string[16];
					wsprintf(string, "FPS:%d", FPS);
					SetWindowText(hWnd, string);
					FPS = 0;
					lastFpsResetTime = nowTime;
				}
			}


			//指定した時間（FPSを60に設定した場合は60分の1秒）経過していたら更新処理
			if ((nowTime - lastUpdateTime) * fpsLimit > 1000.0f)
			{
				//時間計測関連
				lastUpdateTime = nowTime;	//現在の時間（最後に画面を更新した時間）を覚えておく
				FPS++;						//画面更新回数をカウントする




				ImGui_ImplDX11_NewFrame();
				ImGui_ImplWin32_NewFrame();
				ImGui::NewFrame();

				//このフレームの描画開始
				Direct3D::BeginDraw();
				ImDrawList* drawList = ImGui::GetBackgroundDrawList();
				const XMFLOAT3 textureSize = idleFrames.front()->GetSize();
				const float scale = (std::min)(1.0f, (std::min)(
					static_cast<float>(screenWidth) / textureSize.x,
					static_cast<float>(screenHeight) / textureSize.y));
				const ImVec2 imageSize(textureSize.x * scale, textureSize.y * scale);
				const ImVec2 imagePosition(screenWidth - imageSize.x, screenHeight - imageSize.y);
				const size_t frameIndex = static_cast<size_t>((GetTickCount64() * animationFps / 1000) % idleFrames.size());
				if (frameIndex != lastRegionFrame)
				{
					UpdateAlphaWindowRegion(hWnd, *idleFrames[frameIndex], scale, imagePosition);
					lastRegionFrame = frameIndex;
				}
				drawList->AddImage(
					(ImTextureID)idleFrames[frameIndex]->GetSRV(),
					imagePosition,
					ImVec2(imagePosition.x + imageSize.x, imagePosition.y + imageSize.y));

				//ImGuiの描画
				ImGui::Render();
				ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

				//描画終了
				Direct3D::EndDraw();
				RenderSettingsWindow(hWnd);
				if (!pendingCharacterFolder.empty())
				{
					if (LoadIdleFrames(pendingCharacterFolder, idleFrames))
					{
						loadedCharacterFolder = pendingCharacterFolder;
						lastRegionFrame = idleFrames.size();
						voiceStatus = "キャラクターを切り替えたよ。";
					}
					else
					{
						activeCharacterFolder = loadedCharacterFolder;
						characterConfigPath = GetCharacterConfigPath(activeCharacterFolder);
						LoadCharacterVoiceDefaults();
						WritePrivateProfileString("CHARACTER", "Folder", activeCharacterFolder.c_str(), ".\\setup.ini");
						voiceStatus = "画像を読めなかったので、前のキャラクターに戻したよ。";
					}
					pendingCharacterFolder.clear();
				}



				
				//ちょっと休ませる
				Sleep(1);
			}
			timeEndPeriod(1);	//時間計測の制度を戻す
		}
	}

	

	//ImGuiの解放
	if (voiceTask.valid())
	{
		voiceTask.wait();
		voiceTask.get();
	}
	ReleaseSettingsWindow();
	Audio::AllRelease();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();

	idleFrames.clear();
	Direct3D::Release();

	return 0;
}


//ウィンドウの作成
HWND InitApp(HINSTANCE hInstance, int screenWidth, int screenHeight, int nCmdShow)
{
	//ウィンドウクラス（設計図）を作成
	WNDCLASSEX wc;
	wc.cbSize = sizeof(WNDCLASSEX);					//この構造体のサイズ
	wc.hInstance = hInstance;						//インスタンスハンドル
	wc.lpszClassName = WIN_CLASS_NAME;				//ウィンドウクラス名
	wc.lpfnWndProc = WndProc;						//ウィンドウプロシージャ
	wc.style = CS_VREDRAW | CS_HREDRAW;				//スタイル（デフォルト）
	//wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);	//アイコン
	//wc.hIconSm = LoadIcon(nullptr, IDI_WINLOGO);	//小さいアイコン
	wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
	wc.hIconSm = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);	//マウスカーソル
	wc.lpszMenuName = nullptr;						//メニュー（なし）
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);	//背景（白）
	RegisterClassEx(&wc);

	//画面の右下に置いてみるね
	MONITORINFO monitorInfo = {};
	monitorInfo.cbSize = sizeof(monitorInfo);
	HMONITOR monitor = MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
	GetMonitorInfo(monitor, &monitorInfo);
	const int windowX = monitorInfo.rcWork.right - screenWidth;
	const int windowY = monitorInfo.rcWork.bottom - screenHeight;

	//タイトルバーに表示する内容
	char caption[64];
	GetPrivateProfileString("SCREEN", "Caption", "***", caption, 64, ".\\setup.ini");

	//ウィンドウを作成
	HWND hWnd = CreateWindowEx(
		WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW,
		WIN_CLASS_NAME,					//ウィンドウクラス名
		caption,						//タイトルバーに表示する内容
		WS_POPUP,						//枠のないウィンドウにするね
		windowX,						//画面の右下に置くよ
		windowY,
		screenWidth,					//設定した幅で表示するね
		screenHeight,
		nullptr,						//親ウインドウ（なし）
		nullptr,						//メニュー（なし）
		hInstance,						//インスタンス
		nullptr							//パラメータ（なし）
	);

	//ウィンドウを表示
	ShowWindow(hWnd, nCmdShow);

	return hWnd;
}

LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	ImGuiContext* previousContext = ImGui::GetCurrentContext();
	if (settingsImGuiContext != nullptr)
	{
		ImGui::SetCurrentContext(settingsImGuiContext);
		ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
		ImGui::SetCurrentContext(previousContext);
	}
	switch (msg)
	{
	case WM_SIZE:
		ResizeSettingsWindow();
		return 0;
	case WM_CLOSE:
		ShowWindow(hWnd, SW_HIDE);
		if (mascotHWnd != nullptr)
			SetWindowPos(mascotHWnd, isAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		return 0;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

//ウィンドウプロシージャ（何かあった時によばれる関数）
LRESULT CALLBACK WndProc(
	HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(
		hWnd, msg, wParam, lParam))
	{
		return TRUE;
	}

	switch (msg)
	{
	case WM_NCHITTEST:
		return HTCAPTION;

	case WM_CONTEXTMENU:
	case WM_RBUTTONUP:
	case WM_NCRBUTTONUP:
	{
		HMENU menu = CreatePopupMenu();
		if (menu == nullptr)
			return 0;
		AppendMenuW(menu, MF_STRING, MENU_SETTINGS, L"設定");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING | (isAlwaysOnTop ? MF_CHECKED : MF_UNCHECKED), MENU_TOGGLE_TOPMOST, L"常に最前面");
		AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenuW(menu, MF_STRING, MENU_EXIT, L"終了");
		POINT cursor = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
		if (msg == WM_RBUTTONUP)
			ClientToScreen(hWnd, &cursor);
		else if (msg == WM_CONTEXTMENU && cursor.x == -1 && cursor.y == -1)
			GetCursorPos(&cursor);
		const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, hWnd, nullptr);
		DestroyMenu(menu);
		if (command == MENU_TOGGLE_TOPMOST)
		{
			isAlwaysOnTop = !isAlwaysOnTop;
			SetWindowPos(hWnd, isAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			WritePrivateProfileString("WINDOW", "AlwaysOnTop", isAlwaysOnTop ? "1" : "0", ".\\setup.ini");
		}
		else if (command == MENU_EXIT)
		{
			DestroyWindow(hWnd);
		}
		else if (command == MENU_SETTINGS && !OpenSettingsWindow(hWnd))
		{
			MessageBoxW(hWnd, L"設定画面を開けませんでした。", L"起動エラー", MB_OK | MB_ICONERROR);
		}
		return 0;
	}

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	}

	return DefWindowProc(hWnd, msg, wParam, lParam);
}
