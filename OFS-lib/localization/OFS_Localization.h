#pragma once
#include "OFS_StringsGenerated.h"
#include "OFS_Util.h"
#include <array>
#include <vector>
#include <variant>
#include <string>
#include <unordered_map>
#include <unordered_set>

class OFS_Translator
{
    private:
    std::vector<char> StringData;
    // Text translated by its English, for everything written in English in
    // the code rather than given a key in localization.csv. A row of a
    // translation file keyed #TEXT holds the English in its second column
    // and the translation in its third.
    std::unordered_map<std::string, std::string> texts;
    // A translation with the ImGui id the English had after its ##, kept so
    // the pointer handed out stays valid.
    std::unordered_map<std::string, std::string> composed;
    // Every English text asked for this session, for the template a
    // translator fills in, less the keyed strings, which have their own.
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> keyedDefaults;
    
    public:
    static OFS_Translator* ptr;
    static constexpr const char* TranslationDir = "lang";

    OFS_Translator() noexcept;
    std::array<const char*, static_cast<uint32_t>(Tr::MAX_STRING_COUNT)> Translation;
    bool LoadTranslation(const char* name) noexcept;
    void LoadDefaults() noexcept;

    static bool MergeIntoOne(const char* input1, const char* input2, const char* outputPath) noexcept;

    // The translation of text written in English, or the English itself.
    // Anything after ## is an ImGui id and is kept as it is.
    const char* Text(const char* english) noexcept;
    inline static const char* Tx(const char* english) noexcept
    {
        return ptr != nullptr ? ptr->Text(english) : english;
    }
    // Writes every English text seen this session, with any translation the
    // loaded language has for it, as #TEXT rows to add to a translation file.
    bool SaveTextTemplate(const char* path) const noexcept;
    inline size_t TextTranslationCount() const noexcept { return texts.size(); }
    inline const std::unordered_map<std::string, std::string>& Texts() const noexcept { return texts; }

    static void Init() noexcept
    {
        if(ptr != nullptr) return;
        ptr = new OFS_Translator();
    }

    static void Shutdown() noexcept
    {
        if(ptr)
        {
            delete ptr;
            ptr = nullptr;
        }
    }
};

//#define TR(str_id)\
//OFS_DefaultStrings::Default[static_cast<uint32_t>(Tr::str_id)]
//#define TRD(id)\
//OFS_DefaultStrings::Default[static_cast<uint32_t>(id)]
//#define TR_ID(id, str_id)\
//FMT("%s###%s", OFS_DefaultStrings::Default[static_cast<uint32_t>(str_id)], id)

#define TR(str_id)\
OFS_Translator::ptr->Translation[static_cast<uint32_t>(Tr::str_id)]
#define TRD(id)\
OFS_Translator::ptr->Translation[static_cast<uint32_t>(id)]
#define TR_ID(id, str_id)\
FMT("%s###%s", OFS_Translator::ptr->Translation[static_cast<uint32_t>(str_id)], id)
// Text written in English where it is used, translated when the language
// loaded has it.
#define TRS(english) OFS_Translator::Tx(english)

class TrString
{
    private:
    std::variant<Tr, std::string> value = std::string();
    public:
    TrString() noexcept {}
    TrString(const char* str) noexcept
        : value(std::string(str)) {}
    TrString(const std::string& str) noexcept
        : value(str) {}
    TrString(Tr value) noexcept
        : value(value) {}

    inline const char* c_str() const noexcept
    {
        if(auto string = std::get_if<std::string>(&value))
        {
            return string->c_str();
        }
        else if(auto tr = std::get_if<Tr>(&value)) 
        {
            return TRD(*tr);
        }
        
        FUN_ASSERT(false, "unreachable");
        return "";
    }
};