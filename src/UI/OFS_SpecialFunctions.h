#pragma once

#include <memory>
#include "Funscript.h"

#include "state/SpecialFunctionsState.h"

class FunctionBase {
protected:
	inline Funscript& ctx() noexcept;
public:
	virtual ~FunctionBase() noexcept {}
	virtual void DrawUI() noexcept = 0;
	// The tool is being closed or swapped for another.
	virtual void Hidden() noexcept {}
};

class FunctionRangeExtender : public FunctionBase 
{
	int32_t rangeExtend = 0;
	bool createUndoState = true;
	UnsubscribeFn eventUnsub;
public:
	FunctionRangeExtender() noexcept;
	virtual ~FunctionRangeExtender() noexcept;
	void SelectionChanged(const FunscriptSelectionChangedEvent* ev) noexcept;
	virtual void DrawUI() noexcept override;
};

class RamerDouglasPeucker : public FunctionBase
{
	float epsilon = 0.0f;
	float averageDistance = 0.f;
	bool createUndoState = true;
	// How many selection changes simplify has caused itself and not yet seen
	// come back as events, so they are not taken for the user picking
	// something new. A count rather than a flag: dragging selects on every
	// frame, and the events arrive frames later, so several can be on their
	// way at once, and a flag let all but the first reset the tolerance.
	int32_t ownSelectionChangesPending = 0;
	UnsubscribeFn eventUnsub;
public:
	RamerDouglasPeucker() noexcept;
	virtual ~RamerDouglasPeucker() noexcept;
	void SelectionChanged(const FunscriptSelectionChangedEvent* ev) noexcept;
	virtual void DrawUI() noexcept override;
};

// Writes twist, roll and pitch that move with the selected points of a stroke
// script, each axis with its own motion, into the project's axis scripts.
class MultiAxisGenerator : public FunctionBase
{
	uint32_t stateHandle = 0xFFFF'FFFF;
	int32_t shownAxis = 1;
	// A preview is written into the axis scripts, as one undo step, but is not
	// kept until it is applied: a change to a setting replaces it, and Discard,
	// or leaving the tool, takes it back out. It follows the stroke points that
	// were selected when it was started.
	bool previewing = false;
	FunscriptArray previewSource;
	std::string previewAxes;
	// The axes of the last preview that was applied, to say so.
	std::string appliedAxes;
	UnsubscribeFn eventUnsub;
	void generate() noexcept;
	void discardPreview() noexcept;
public:
	MultiAxisGenerator(uint32_t stateHandle) noexcept;
	virtual ~MultiAxisGenerator() noexcept;
	void SelectionChanged(const FunscriptSelectionChangedEvent* ev) noexcept;
	virtual void DrawUI() noexcept override;
	virtual void Hidden() noexcept override;
};

// Writes strokes along the beat across a chapter, a selection, or every
// chapter with music in it, using the tempo detection recorded on the chapter
// and the downbeat it found. Either a plain up and down stroke, or a passage
// saved earlier as a pattern and stamped repeatedly.
class BeatFill : public FunctionBase
{
	uint32_t stateHandle = 0xFFFF'FFFF;
	uint32_t tempoStateHandle = 0xFFFF'FFFF;
	// What the last fill did, so the button can say so rather than appearing
	// to do nothing when the range had no tempo to work from.
	std::string lastResult;
	bool lastResultIsError = false;
	// The name being typed for a pattern about to be saved.
	std::string patternName;
	bool savingPattern = false;

	void fill() noexcept;
	void savePattern() noexcept;
public:
	BeatFill(uint32_t stateHandle) noexcept;
	virtual ~BeatFill() noexcept;
	virtual void DrawUI() noexcept override;
};

// Scales the selected strokes by how loud the audio is under them, so a quiet
// passage is scripted shallower than a loud one without redrawing it.
class LoudnessDepth : public FunctionBase
{
	uint32_t stateHandle = 0xFFFF'FFFF;
	std::string lastResult;
	bool lastResultIsError = false;

	void apply() noexcept;
public:
	LoudnessDepth(uint32_t stateHandle) noexcept;
	virtual ~LoudnessDepth() noexcept;
	virtual void DrawUI() noexcept override;
};

class SpecialFunctionsWindow {
private:
	FunctionBase* function = nullptr;
	uint32_t stateHandle = 0xFFFF'FFFF;
public:
	static constexpr const char* WindowId = "###SPECIAL_FUNCTIONS";
	SpecialFunctionsWindow() noexcept;
	void SetFunction(SpecialFunctionType function) noexcept;
	void ShowFunctionsWindow(bool* open) noexcept;
};