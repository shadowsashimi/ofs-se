#include "ChapterState.h"

#include <algorithm>
#include <cmath>

inline static bool checkForOverlapChapters(const std::vector<Chapter>& chapters, float startTime, float endTime, const Chapter* exclude = nullptr) noexcept
{
    for(int i=0, size=chapters.size(); i < size; i += 1)
    {
        auto& chapter = chapters[i];
        if(exclude == &chapter) continue;
        // Half open: a chapter runs from its start up to but not including its
        // end, so two that meet at an instant do not overlap. That instant
        // belongs to the later one, which is the same rule the tempo grid uses
        // when it asks which chapter the playhead is in.
        if(startTime < chapter.endTime && chapter.startTime < endTime)
        {
            return true;
        }
    }
    return false;
}

bool ChapterState::SetChapterSize(Chapter& chapter, float toTime) noexcept
{
    const float testStartTime = toTime < chapter.startTime ? toTime : chapter.startTime;
    const float testEndTime = toTime > chapter.endTime ? toTime : chapter.endTime;

    if(toTime >= chapter.startTime && toTime <= chapter.endTime)
    {
        // intersects with itself means it should shrink
        if(std::abs(toTime - chapter.startTime) < std::abs(toTime - chapter.endTime))
        {
            chapter.startTime = toTime;
            return true;
        }
        else
        {
            chapter.endTime = toTime;
            return true;
        }
    }
    else if(checkForOverlapChapters(chapters, testStartTime, testEndTime, &chapter))
    {
        // check if truncating toTime is possible
        auto it = std::find_if(chapters.begin(), chapters.end(),
            [&](auto& c) noexcept { return &c == &chapter; }); 
        if(it != chapters.end())
        {
            if(toTime < chapter.startTime)
            {
                // check left neighbour
                if(it == chapters.begin())
                    return false;
                auto n = it - 1;
                toTime = std::nextafter(n->endTime, chapter.startTime);
            }
            else if(toTime > chapter.endTime)
            {
                // check right neighbour
                auto n = it + 1;
                if(n == chapters.end()) 
                    return false;
                toTime = std::nextafter(n->startTime, chapter.startTime);
            }
        }
    }

    if(toTime < chapter.startTime)
    {
        // grow left
        chapter.startTime = toTime;
        return true;
    }
    else if(toTime > chapter.endTime)
    {
        // grow right
        chapter.endTime = toTime;
        return true;
    }


    return false;
}

// Moves the boundary between this chapter and the one before it.
//
// The boundary is one thing rather than two kept in step by hand: a chapter
// start that runs into the previous chapter carries that chapter's end along
// with it. Where the two were not touching to begin with, the gap between them
// is left as it was.
bool ChapterState::SetChapterStart(Chapter& chapter, float toTime) noexcept
{
    if(toTime >= chapter.endTime) return false;

    auto it = std::find_if(chapters.begin(), chapters.end(),
        [&](auto& c) noexcept { return &c == &chapter; });
    if(it == chapters.end()) return false;

    if(it != chapters.begin())
    {
        auto previous = it - 1;
        // The neighbour has to keep some length of its own.
        if(toTime <= previous->startTime) return false;
        const bool wereTouching = std::abs(previous->endTime - chapter.startTime) < 0.001f;
        if(toTime < previous->endTime || wereTouching) previous->endTime = toTime;
    }

    chapter.startTime = toTime;
    return true;
}

// The same, for the boundary with the chapter after this one.
bool ChapterState::SetChapterEnd(Chapter& chapter, float toTime) noexcept
{
    if(toTime <= chapter.startTime) return false;

    auto it = std::find_if(chapters.begin(), chapters.end(),
        [&](auto& c) noexcept { return &c == &chapter; });
    if(it == chapters.end()) return false;

    auto next = it + 1;
    if(next != chapters.end())
    {
        if(toTime >= next->endTime) return false;
        const bool wereTouching = std::abs(next->startTime - chapter.endTime) < 0.001f;
        if(toTime > next->startTime || wereTouching) next->startTime = toTime;
    }

    chapter.endTime = toTime;
    return true;
}

Chapter* ChapterState::AddChapter(float time, float duration) noexcept
{
    float startTime = time;
    float endTime = startTime + (0.01f * duration);

    if(checkForOverlapChapters(chapters, startTime, endTime))
    {
        return nullptr;
    }

    Chapter newChapter = {0};
    newChapter.startTime = startTime;
    newChapter.endTime = endTime;
    newChapter.color = Util::RandomColor(0.65f, 0.70f);
    // A name to start from. A chapter made by hand came out nameless, an
    // empty field in the chapter panel and an unlabelled sliver on the
    // timeline, with nothing to say it was the one just made. Detection
    // names the chapters it creates itself.
    newChapter.name = "Chapter " + std::to_string(chapters.size() + 1);

    if(chapters.empty())
    {
        auto& c = chapters.emplace_back(std::move(newChapter));
        return &c;
    }
    else 
    {
        for(int i=0, size=chapters.size(); i < size; i += 1)
        {
            auto& chapter = chapters[i];
            if(chapter.startTime >= newChapter.endTime)
            {
                // insert before this chapter
                auto it = chapters.insert(chapters.begin() + i, std::move(newChapter));
                return &(*it);
            }
        }
        auto& c = chapters.emplace_back(std::move(newChapter));
        return &c;
    }

    return nullptr;
}

Bookmark* ChapterState::AddBookmark(float time) noexcept
{
    for(auto& bookmark : bookmarks)
    {
        // Minimum time between bookmarks is 1 second
        if(std::abs(bookmark.time - time) <= 1.f)
        {
            return nullptr;
        }
    }
    auto& newBookmark = bookmarks.emplace_back();
    newBookmark.time = time;
    return &newBookmark;
}

std::string Chapter::StartTimeToString() const noexcept
{
    char tmpBuf[16];
    int size = Util::FormatTime(tmpBuf, sizeof(tmpBuf), startTime, true);
    return std::string(tmpBuf, size);
}

std::string Chapter::EndTimeToString() const noexcept
{
    char tmpBuf[16];
    int size = Util::FormatTime(tmpBuf, sizeof(tmpBuf), endTime, true);
    return std::string(tmpBuf, size);    
}

std::string Bookmark::TimeToString() const noexcept
{
    char tmpBuf[16];
    int size = Util::FormatTime(tmpBuf, sizeof(tmpBuf), time, true);
    return std::string(tmpBuf, size);
}