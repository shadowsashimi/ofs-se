#include "OFS_Localization.h"
#include "OFS_Util.h"
#include "rapidcsv.h"

#include <optional>

OFS_Translator* OFS_Translator::ptr = nullptr;

OFS_Translator::OFS_Translator() noexcept
{
    Util::CreateDirectories(Util::Prefpath(TranslationDir));
    // initialize with the default strings
    LoadDefaults();
}

void OFS_Translator::LoadDefaults() noexcept
{
    StringData = std::vector<char>();
    texts.clear();
    composed.clear();
    keyedDefaults.clear();
    for (const char* text : OFS_DefaultStrings::Default) {
        if (text != nullptr) keyedDefaults.insert(text);
    }
    memcpy(Translation.data(), OFS_DefaultStrings::Default.data(), Translation.size() * sizeof(const char*));
}

static std::optional<rapidcsv::Document> OpenDocument(const char* path) noexcept
{
    try
    {
        return rapidcsv::Document(path, 
            rapidcsv::LabelParams(),
            rapidcsv::SeparatorParams(',', false, true, true),
            rapidcsv::ConverterParams(),
            rapidcsv::LineReaderParams()
        );
    }
    catch(const std::exception& e)
    {
        LOG_ERROR(e.what());
        return std::optional<rapidcsv::Document>();
    }
}

bool OFS_Translator::LoadTranslation(const char* name) noexcept
{
    auto stdPath = Util::PathFromString(Util::Prefpath(TranslationDir)) / name;
    auto path = stdPath.u8string();

    auto docOpt = OpenDocument(path.c_str());
    if(!docOpt.has_value()) return false;

    auto doc = docOpt.value();
    std::vector<char> tmpData;
    tmpData.reserve(1024*15);

    std::vector<const char*> tmpTranslation;
    tmpTranslation.resize(OFS_DefaultStrings::Default.size());
    
    std::vector<int> tmpTrIndices;
    tmpTrIndices.resize(OFS_DefaultStrings::Default.size(), -1);
    std::unordered_map<std::string, std::string> tmpTexts;

    auto row = doc.GetRow<std::string>(0);
    if(row.size() != 3) {        
        LOG_ERROR("Translation column count mismatch.");
        return false;
    }

    for(size_t i=1; i < doc.GetRowCount(); i += 1)
    {
        row = doc.GetRow<std::string>(i);
        if(row.size() != 3) {
            continue;
        }

        auto key = std::move(row[0]);
        auto value = std::move(row[2]);
        value = Util::trim(value);

        if(value.empty()) {
            continue;
        }
        if(key == "#TEXT") {
            if(!row[1].empty()) tmpTexts[row[1]] = value;
            continue;
        }

        auto it = OFS_DefaultStrings::KeyMapping.find(key);
        if(it != OFS_DefaultStrings::KeyMapping.end())
        {
            size_t index = static_cast<size_t>(it->second);
            size_t valPos = tmpData.size();
            tmpTrIndices[index] = valPos;

            for(auto c : value) {
                tmpData.emplace_back(c);
            }
            tmpData.emplace_back('\0');
        }
    }

    for(int i=0; i < OFS_DefaultStrings::Default.size(); i += 1)
    {
        if(tmpTrIndices[i] >= 0) {
            tmpTranslation[i] = &tmpData[tmpTrIndices[i]];
        }
        else 
        {
            tmpTranslation[i] = OFS_DefaultStrings::Default[i];
        }
    }

    StringData = std::move(tmpData);
    memcpy(Translation.data(), tmpTranslation.data(), Translation.size() * sizeof(const char*));
    texts = std::move(tmpTexts);
    composed.clear();
    // The keyed strings as this language has them, so they are not taken
    // for text to translate when they pass through a helper that asks.
    for (const char* text : Translation) {
        if (text != nullptr) keyedDefaults.insert(text);
    }
    LOGF_INFO("Loaded %d keyed and %d text translations from %s", (int)Translation.size(), (int)texts.size(), name);
    return true;
}

const char* OFS_Translator::Text(const char* english) noexcept
{
    if (english == nullptr || english[0] == '\0') return english;
    const char* idAt = strstr(english, "##");
    const std::string visible = idAt ? std::string(english, idAt) : std::string(english);
    if (visible.empty()) return english;
    if (keyedDefaults.find(visible) == keyedDefaults.end()) seen.insert(visible);
    if (texts.empty()) return english;
    auto it = texts.find(visible);
    if (it == texts.end()) return english;
    if (idAt == nullptr) return it->second.c_str();
    auto& whole = composed[english];
    if (whole.empty()) whole = it->second + idAt;
    return whole.c_str();
}

static std::string csvField(const std::string& text) noexcept
{
    std::string out = "\"";
    for (char c : text) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

bool OFS_Translator::SaveTextTemplate(const char* path) const noexcept
{
    std::vector<std::string> all(seen.begin(), seen.end());
    for (const auto& [english, translation] : texts) {
        if (seen.find(english) == seen.end()) all.push_back(english);
    }
    std::sort(all.begin(), all.end());
    std::string out = "Key (do not touch),Default,Translation\n";
    for (const auto& english : all) {
        auto it = texts.find(english);
        out += "#TEXT," + csvField(english) + "," + csvField(it != texts.end() ? it->second : std::string()) + "\n";
    }
    return Util::WriteFile(path, out.data(), out.size()) == out.size();
}

bool OFS_Translator::MergeIntoOne(const char* inputPath1, const char* inputPath2, const char* outputPath) noexcept
{
    /*
        This function takes input1 and merges input2 into it.
        The merged csv gets written to outputPath.
    */
    auto inputOpt1 = OpenDocument(inputPath1);
    auto inputOpt2 = OpenDocument(inputPath2);
    if(!inputOpt1.has_value() || !inputOpt2.has_value()) return false;
    auto input1 = std::move(inputOpt1.value());
    auto input2 = std::move(inputOpt2.value());

    auto output = rapidcsv::Document(std::string(),
        rapidcsv::LabelParams(),
        rapidcsv::SeparatorParams(',', false, true, true),
        rapidcsv::ConverterParams(),
        rapidcsv::LineReaderParams()
    );
    
    std::array<const char*, static_cast<int>(Tr::MAX_STRING_COUNT)> lut;
    for(auto& mapping : OFS_DefaultStrings::KeyMapping) {
        lut[static_cast<int>(mapping.second)] = mapping.first.c_str();
    }

    std::array<std::string, static_cast<int>(Tr::MAX_STRING_COUNT)> input1Lut;
    for(size_t i=0, size=input1.GetRowCount(); i < size; i += 1) {
        auto row = input1.GetRow<std::string>(i);
        if(row.size() < 3) return false;
        auto it = OFS_DefaultStrings::KeyMapping.find(row[0]);
        if(it != OFS_DefaultStrings::KeyMapping.end()) {
            input1Lut[static_cast<int>(it->second)] = row[2];
        }
    }

    std::array<std::string, static_cast<int>(Tr::MAX_STRING_COUNT)> input2Lut;
    for(size_t i=0, size=input2.GetRowCount(); i < size; i += 1) {
        auto row = input2.GetRow<std::string>(i);
        if(row.size() < 3) return false;
        auto it = OFS_DefaultStrings::KeyMapping.find(row[0]);
        if(it != OFS_DefaultStrings::KeyMapping.end()) {
            input2Lut[static_cast<int>(it->second)] = row[2];
        }
    }

    std::vector<std::string> row = {
        "Key (do not touch)",
        "Default",
        "Translation",
    };
    output.SetColumnName(0, row[0]);
    output.SetColumnName(1, row[1]);
    output.SetColumnName(2, row[2]);

    for(int idx = 0; idx < static_cast<int>(Tr::MAX_STRING_COUNT); idx += 1) {
        Tr current = static_cast<Tr>(idx);
        row[0] = lut[idx];
        row[1] = TRD(current);
        row[2] = input1Lut[idx].empty() ? input2Lut[idx] : input1Lut[idx];
        output.InsertRow(output.GetRowCount(), row);
    }

    try 
    {
        output.Save(outputPath);
    }
    catch(std::exception ex) 
    {
        LOG_ERROR(ex.what());
        return false;
    }
    return true;
}
