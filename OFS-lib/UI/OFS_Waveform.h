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
public:

	inline bool BusyGenerating() noexcept { return generating; }
	// bassOnly keeps the low end alone, where the beat usually is.
	bool GenerateAndLoadFlac(const std::string& ffmpegPath, const std::string& videoPath,
		const std::string& output, bool bassOnly = false) noexcept;
	bool LoadFlac(const std::string& path) noexcept;

	inline void Clear() noexcept {
		samples.clear();
	}

	inline void SetSamples(std::vector<float>&& samples) noexcept
	{
		this->samples = std::move(samples);
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

	OFS_Waveform data;

	void Init() noexcept;
	void Update(const struct OverlayDrawingCtx& ctx) noexcept;
	void Upload() noexcept;
};