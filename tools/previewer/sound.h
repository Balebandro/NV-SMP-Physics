#pragma once
// The previewer's impact sounds (the plugin's <impact-sound>, see SkyrimPhysicsWorld::contactsFound): each WAV read
// once and played with XAudio2, several at once, at the impact's volume. PCM only (the game's own sound effects are).
#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

namespace sound
{
	struct Clip
	{
		WAVEFORMATEX format{};
		std::vector<uint8_t> data;
		std::string error;   // why it does not play (empty if it does)
	};

	inline IXAudio2* gAudio = nullptr;
	inline IXAudio2MasteringVoice* gMaster = nullptr;
	inline bool gTried = false;
	inline std::vector<IXAudio2SourceVoice*> gVoices;   // playing, or done and not yet destroyed
	inline std::map<std::string, Clip> gClips;          // by path on disk
	static constexpr size_t kMaxVoices = 16;

	// The audio device, made on first use. False if there is none.
	inline bool Ready()
	{
		if (gTried) return gMaster != nullptr;
		gTried = true;
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // already initialised another way: fine either way
		if (FAILED(XAudio2Create(&gAudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) { gAudio = nullptr; return false; }
		if (FAILED(gAudio->CreateMasteringVoice(&gMaster))) { gMaster = nullptr; return false; }
		return true;
	}

	// A RIFF WAVE file's format and samples.
	inline Clip Read(const std::string& path)
	{
		Clip c;
		FILE* f = fopen(path.c_str(), "rb");
		if (!f) { c.error = "cannot open it"; return c; }
		std::vector<uint8_t> b;
		fseek(f, 0, SEEK_END);
		b.resize((size_t)(std::max)(0L, ftell(f)));
		fseek(f, 0, SEEK_SET);
		if (!b.empty() && fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
		fclose(f);
		if (b.size() < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) { c.error = "not a WAV file"; return c; }
		bool haveFormat = false;
		for (size_t o = 12; o + 8 <= b.size();) {
			uint32_t size;
			memcpy(&size, b.data() + o + 4, 4);
			const size_t body = o + 8, end = (std::min)(b.size(), body + size);
			if (!memcmp(b.data() + o, "fmt ", 4) && end - body >= 16) {
				memcpy(&c.format, b.data() + body, (std::min)(end - body, sizeof(WAVEFORMATEX)));
				c.format.cbSize = 0;
				haveFormat = true;
			} else if (!memcmp(b.data() + o, "data", 4))
				c.data.assign(b.begin() + body, b.begin() + end);
			o = body + size + (size & 1);
		}
		if (!haveFormat || c.data.empty()) c.error = "no format or no samples in it";
		else if (c.format.wFormatTag != WAVE_FORMAT_PCM) c.error = "not PCM (format " + std::to_string(c.format.wFormatTag) + ")";
		if (!c.error.empty()) c.data.clear();
		return c;
	}

	// Plays the file on disk once at `volume` (0..1). An empty string if it plays, else why not.
	inline std::string Play(const std::string& path, float volume)
	{
		if (!Ready()) return "no audio device";
		// Voices that finished go first.
		for (auto it = gVoices.begin(); it != gVoices.end();) {
			XAUDIO2_VOICE_STATE s;
			(*it)->GetState(&s, XAUDIO2_VOICE_NOSAMPLESPLAYED);
			if (s.BuffersQueued == 0) { (*it)->DestroyVoice(); it = gVoices.erase(it); }
			else ++it;
		}
		if (gVoices.size() >= kMaxVoices) return "too many sounds at once";
		auto it = gClips.find(path);
		if (it == gClips.end()) it = gClips.emplace(path, Read(path)).first;
		const Clip& c = it->second;
		if (!c.error.empty()) return c.error;
		IXAudio2SourceVoice* v = nullptr;
		if (FAILED(gAudio->CreateSourceVoice(&v, &c.format))) return "the audio device refused its format";
		XAUDIO2_BUFFER buffer{};
		buffer.Flags = XAUDIO2_END_OF_STREAM;
		buffer.AudioBytes = (UINT32)c.data.size();
		buffer.pAudioData = c.data.data();
		v->SubmitSourceBuffer(&buffer);
		v->SetVolume(volume);
		v->Start();
		gVoices.push_back(v);
		return {};
	}
}
