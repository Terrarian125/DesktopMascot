#pragma once

#include <Windows.h>
#include <string>
#include <vector>

namespace Voicevox
{
	enum class EngineStartResult
	{
		AlreadyRunning,
		Started,
		NotFound,
		Failed
	};

	struct Speaker
	{
		int id = 0;
		std::string label;
	};

	struct TaskResult
	{
		bool success = false;
		std::string error;
		std::vector<Speaker> speakers;
		std::vector<BYTE> wavData;
	};

	TaskResult FetchSpeakers();
	TaskResult SynthesizeTest(int speakerId, float speed, float pitch, float volume);
	EngineStartResult EnsureEngineStarted();
}
