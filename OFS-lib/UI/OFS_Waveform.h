#pragma once

#include <vector>
#include <string>
#include <memory>

#include "OFS_BinarySerialization.h"
#include "OFS_Shader.h"
#include "imgui.h"



// helper class to render audio waves
class OFS_Waveform
{
	bool generating = false;
	std::vector<float> samples;
	// Goes up whenever the samples change. What is drawn from them, and the
	// beat ticks found in them, are made again when it does. They used to be
	// made again when the sample count changed, which another waveform of the
	// same file, its bass or one channel of it, does not do, so switching
	// between them showed the old picture.
	uint32_t version = 0;
public:
	inline uint32_t Version() const noexcept { return version; }

	inline bool BusyGenerating() noexcept { return generating; }
	// bassOnly keeps the low end alone, where the beat usually is.
	// channel: 0 both mixed, 1 the left alone, 2 the right alone.
	bool GenerateAndLoadFlac(const std::string& ffmpegPath, const std::string& videoPath,
		const std::string& output, bool bassOnly = false, int32_t channel = 0) noexcept;
	bool LoadFlac(const std::string& path) noexcept;

	inline void Clear() noexcept {
		samples.clear();
		version += 1;
	}

	inline void SetSamples(std::vector<float>&& samples) noexcept
	{
		this->samples = std::move(samples);
		version += 1;
	}

	inline const std::vector<float>& Samples() const noexcept { return samples; }

	inline size_t SampleCount() const noexcept {
		return samples.size();
	}
};

struct OFS_WaveformLOD
{
	std::vector<float> WaveformLineBuffer;
	std::unique_ptr<WaveformShader> WaveShader;
	ImColor WaveformColor = IM_COL32(0x55, 0x55, 0x55, 255);
	uint32_t WaveformTex = 0;

	// How the canvas maps onto the texture: u = Frag_UV.x * scale + offset.
	// Both are recomputed every frame, which is what makes sub-texel panning
	// smooth without needing to rebuild the texture.
	float samplingOffset = 0.f;
	float samplingScale = 1.f;

	// The buffer is anchored to a global sample grid where texel k always
	// covers samples [k*everyNth, (k+1)*everyNth). Caching against that grid
	// rather than against the current view is what keeps the waveform from
	// sliding when zooming, panning or resizing. Initial values are chosen so
	// that the first Update() always rebuilds.
	int32_t lastEveryNth = 0;
	int32_t lastFirstTexel = 0;
	int32_t lastSampleCount = -1;
	uint32_t lastVersion = 0xFFFF'FFFF;

	OFS_Waveform data;

	void Init() noexcept;
	void Update(const struct OverlayDrawingCtx& ctx) noexcept;
	void Upload() noexcept;
};