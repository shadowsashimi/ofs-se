#include "OFS_Waveform.h"
#include "OFS_Util.h"
#include "OFS_Profiling.h"
#include "OFS_GL.h"
#include "OFS_ScriptTimeline.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#include "subprocess.h"

#include <cmath>
#include <cstring>

bool OFS_Waveform::LoadFlac(const std::string& output) noexcept
{
	drflac* flac = drflac_open_file(output.c_str(), NULL);
	if (!flac) return false;

	std::vector<drflac_int16> ChunkSamples; ChunkSamples.resize(48000);
	constexpr int SamplesPerLine = 300; 

	float minSample = 0.f;
	float maxSample = 0.f;

	uint32_t sampleCount = 0;
	float avgSample = 0.f;
	Clear();
	samples.reserve(flac->totalPCMFrameCount / SamplesPerLine);
	while ((sampleCount = drflac_read_pcm_frames_s16(flac, ChunkSamples.size(), ChunkSamples.data())) > 0) {
		for (int sampleIdx = 0; sampleIdx < sampleCount; sampleIdx += SamplesPerLine) {
			int samplesInThisLine = std::min(SamplesPerLine, (int)sampleCount - sampleIdx);
			for (int i = 0; i < samplesInThisLine; i += 1) {
				drflac_int16 sample = ChunkSamples[sampleIdx + i];
				sample = std::abs(sample);
				auto floatSample = sample / 32768.f;
				avgSample += floatSample;
			}
			avgSample /= (float)SamplesPerLine;
			minSample = Util::Min(minSample, avgSample);
			maxSample = Util::Max(maxSample, avgSample);
			samples.emplace_back(avgSample);
			avgSample = 0.f;
		}
	}
	drflac_close(flac);
	samples.shrink_to_fit();

	if(std::abs(minSample) > std::abs(maxSample)) {
		maxSample = std::abs(minSample);
	}
	else {
		minSample = -maxSample;
	}

	for(auto& sample : samples) {
		sample = Util::MapRange(sample, minSample, maxSample, -1.f, 1.f);
	}

	return true;
}

bool OFS_Waveform::GenerateAndLoadFlac(const std::string& ffmpegPath, const std::string& videoPath,
	const std::string& output, bool bassOnly) noexcept
{
	generating = true;

	// Everything below 150 Hz, which in most music is the kick drum and the
	// bass line: what a stroke usually follows. The rest of the mix - vocals,
	// hats, synths - crowds the envelope and hides that pulse.
	std::vector<const char*> args =
	{
		ffmpegPath.c_str(),
		"-y",
		"-loglevel",
		"quiet",
		"-i", videoPath.c_str(),
		"-vn",
		"-ac", "1",
	};
	if (bassOnly) {
		args.push_back("-af");
		args.push_back("lowpass=f=150");
	}
	args.push_back(output.c_str());
	args.push_back(nullptr);
	struct subprocess_s proc;
	if(subprocess_create(args.data(), subprocess_option_no_window, &proc) != 0) {
		generating = false; 
		return false; 
	}

	if(proc.stdout_file) 
	{
		fclose(proc.stdout_file);
		proc.stdout_file = nullptr;
	}
	
	if(proc.stderr_file) 
	{
		fclose(proc.stderr_file);
		proc.stderr_file = nullptr;
	}

	int return_code;
	subprocess_join(&proc, &return_code);
	subprocess_destroy(&proc);

	if (!LoadFlac(output)) {
		generating = false;
		return false;
	}

	generating = false;
	return true;
}

void OFS_WaveformLOD::Init() noexcept
{
	glGenTextures(1, &WaveformTex);
	glBindTexture(GL_TEXTURE_2D, WaveformTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); 
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	WaveShader = std::make_unique<WaveformShader>();
}

// Flip to 1 to get a live panel of the mapping maths while working on this.
#define OFS_WAVEFORM_DEBUG 0

void OFS_WaveformLOD::Update(const OverlayDrawingCtx& ctx) noexcept
{
	OFS_PROFILE(__FUNCTION__);

	const auto& samples = data.Samples();
	const int32_t totalSampleCount = (int32_t)samples.size();
	if(totalSampleCount <= 0 || ctx.totalDuration <= 0.f || ctx.canvasSize.x <= 0.f) {
		return;
	}

	const float relStart = ctx.offsetTime / ctx.totalDuration;
	const float relDuration = ctx.visibleTime / ctx.totalDuration;

	const float startIndexF = relStart * totalSampleCount;
	const float endIndexF = startIndexF + (relDuration * totalSampleCount);
	const float visibleSampleCountF = endIndexF - startIndexF;

	const float desiredSamples = ctx.canvasSize.x / 3.f;
	const int32_t everyNth = Util::Max(1, (int32_t)SDL_ceilf(visibleSampleCountF / desiredSamples));

	// Texel k always covers samples [k*everyNth, (k+1)*everyNth). Because this
	// grid is global rather than relative to the current view, texels retained
	// across a scroll stay exactly correct instead of drifting out of phase.
	const int32_t firstTexel = (int32_t)SDL_floorf(startIndexF / everyNth);
	const int32_t lastTexel = (int32_t)SDL_floorf(endIndexF / everyNth);
	// +1 to include lastTexel, +1 guard texel so GL_LINEAR always has a
	// neighbour to interpolate towards at the right edge.
	const int32_t texelCount = (lastTexel - firstTexel) + 2;

	auto& lineBuf = WaveformLineBuffer;

	auto bucket = [&](int32_t texel) noexcept -> float
	{
		float maxSample = 0.f;
		const int32_t begin = texel * everyNth;
		const int32_t end = begin + everyNth;
		for(int32_t i = begin; i < end; i += 1) {
			if(i >= 0 && i < totalSampleCount) {
				maxSample = Util::Max(maxSample, std::abs(samples[i]));
			}
		}
		return maxSample;
	};

	const bool layoutChanged = everyNth != lastEveryNth
		|| texelCount != (int32_t)lineBuf.size()
		|| totalSampleCount != lastSampleCount;
	const int32_t scrollBy = firstTexel - lastFirstTexel;

	if(layoutChanged) {
		OFS_PROFILE("WaveformRebuild");
		lineBuf.resize(texelCount);
		for(int32_t k = 0; k < texelCount; k += 1) {
			lineBuf[k] = bucket(firstTexel + k);
		}
		Upload();
	}
	else if(scrollBy != 0) {
		OFS_PROFILE("WaveformScrolling");
		if(scrollBy > 0 && scrollBy < texelCount) {
			std::memmove(lineBuf.data(), lineBuf.data() + scrollBy,
				sizeof(float) * (texelCount - scrollBy));
			for(int32_t k = texelCount - scrollBy; k < texelCount; k += 1) {
				lineBuf[k] = bucket(firstTexel + k);
			}
		}
		else if(scrollBy < 0 && -scrollBy < texelCount) {
			const int32_t shift = -scrollBy;
			std::memmove(lineBuf.data() + shift, lineBuf.data(),
				sizeof(float) * (texelCount - shift));
			for(int32_t k = 0; k < shift; k += 1) {
				lineBuf[k] = bucket(firstTexel + k);
			}
		}
		else {
			// Jumped further than the buffer is wide, nothing worth keeping.
			for(int32_t k = 0; k < texelCount; k += 1) {
				lineBuf[k] = bucket(firstTexel + k);
			}
		}
		Upload();
	}

	lastEveryNth = everyNth;
	lastFirstTexel = firstTexel;
	lastSampleCount = totalSampleCount;

	// Map the canvas onto the slice of the texture it actually covers. Sampling
	// at texel centres (the +0.5) lines each bucket up with the audio it came
	// from instead of half a texel to the left.
	const float texelCountF = (float)texelCount;
	samplingScale = visibleSampleCountF / (everyNth * texelCountF);
	samplingOffset = ((startIndexF / everyNth) - firstTexel + 0.5f) / texelCountF;

#if OFS_WAVEFORM_DEBUG
	ImGui::Begin("Waveform Debug");
	ImGui::Text("everyNth: %d", everyNth);
	ImGui::Text("texels: %d (first %d, last %d)", texelCount, firstTexel, lastTexel);
	ImGui::Text("sample range: %.2f .. %.2f (%.2f visible)", startIndexF, endIndexF, visibleSampleCountF);
	ImGui::Text("scale: %.6f  offset: %.6f", samplingScale, samplingOffset);
	ImGui::Text("u at left edge:  %.6f", samplingOffset);
	ImGui::Text("u at right edge: %.6f", samplingScale + samplingOffset);
	ImGui::End();
#endif
}

void OFS_WaveformLOD::Upload() noexcept
{
	OFS_PROFILE(__FUNCTION__);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, WaveformTex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, WaveformLineBuffer.size(), 1, 0, GL_RED, GL_FLOAT, WaveformLineBuffer.data());
}