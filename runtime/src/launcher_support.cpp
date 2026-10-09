#include "vita_launcher.h"
#include "disc_image.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace VitaLauncher {
namespace {
uint32_t Word(std::span<const uint8_t> data, size_t at) {
    if (at > data.size() || data.size() - at < 4) throw std::runtime_error("Truncated quest data");
    return uint32_t(data[at]) << 24 | uint32_t(data[at+1]) << 16 | uint32_t(data[at+2]) << 8 | data[at+3];
}
void PutWord(std::vector<uint8_t>& data, size_t at, uint32_t value) {
    for (unsigned i=0;i<4;++i) data.at(at+i)=value >> (24-8*i);
}
void PutHalf(std::vector<uint8_t>& data, size_t at, uint16_t value) { data.at(at)=value>>8; data.at(at+1)=value; }
const char* QuestPath(QuestCatalog::Set set) {
    switch(set) {
    case QuestCatalog::Set::Village: return "05/quest00_eng.bin";
    case QuestCatalog::Set::Online: return "05/quest01_eng.bin";
    case QuestCatalog::Set::Arena: return "05/quest03_eng.bin";
    }
    throw std::runtime_error("Invalid quest category");
}
std::filesystem::path Directory(const std::filesystem::path& user) { return user/"custom-quests"; }
std::string Key(const CustomQuest& q) { return std::to_string(unsigned(q.set))+"-"+std::to_string(q.base); }
void Validate(const CustomQuest& q) {
    if (q.name.empty() || q.name.size()>43 || q.name.find_first_of("\r\n\t")!=q.name.npos ||
        !q.base || !q.count || q.count>99 || q.stars<1 || q.stars>7 || unsigned(q.set)>2)
        throw std::runtime_error("Invalid custom quest settings");
    for(unsigned char c:q.name) if(c<32 || c>126) throw std::runtime_error("Quest name must use printable Latin characters");
    for(unsigned char c:q.storageKey) if((c<'0' || c>'9') && c!='-') throw std::runtime_error("Invalid custom quest storage key");
}
}
bool ValidSave(std::span<const uint8_t> bytes, unsigned character) {
    constexpr size_t size=0x14000, records=0x48, recordSize=0x6000;
    if(bytes.size()!=size || character>=3) return false;
    return (Word(bytes,0)&1) && Word(bytes,records+character*recordSize)!=0 &&
        std::accumulate(bytes.begin()+8,bytes.end(),uint32_t(0))==Word(bytes,4);
}
std::vector<SaveSlot> ProbeSaves(const std::filesystem::path& user) {
    std::vector<SaveSlot> slots;
    const auto directory=user/"nand/title/00010000/524d4850/data/nocopy";
    std::array<uint8_t,0x14000> bytes{};
    for(unsigned file=0;file<6;++file) {
        const auto path=directory/("data0"+std::to_string(file));
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        if(!input || input.tellg()!=std::streampos(bytes.size())) continue;
        input.seekg(0); input.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
        if(!input) continue;
        for(unsigned character=0;character<3;++character)
            if(ValidSave(bytes,character)) slots.push_back({file,character});
    }
    return slots;
}
std::vector<uint8_t> EditQuestFile(std::span<const uint8_t> source,const CustomQuest& q) {
    Validate(q);
    if(source.size()<8 || Word(source,0)!=0x4f4d4f4d) throw std::runtime_error("Invalid quest container");
    const uint32_t count=Word(source,4);
    if(count>(source.size()-8)/8) throw std::runtime_error("Invalid quest table");
    std::vector<uint8_t> edited(source.begin(),source.end());
    for(uint32_t i=0;i<count;++i) {
        const size_t at=Word(source,8+i*8), size=Word(source,12+i*8);
        if(size<0x4b8 || at>source.size() || size>source.size()-at) throw std::runtime_error("Invalid quest extent");
        if((Word(source,at+0x2c)>>16)!=q.base) continue;
        // Retain the template's maps, spawn tables, supplies and rewards. Only
        // supported hunt/capture objectives may be changed by this editor.
        const uint32_t objective=Word(source,at+0x330);
        if(objective!=1 && objective!=0x81) throw std::runtime_error("Choose a hunt/capture quest template");
        const uint16_t templateCount=uint16_t(Word(source,at+0x334));
        if(q.count!=templateCount)throw std::runtime_error("Preserve the template's target count; arbitrary counts need additional spawn scripting");
        const auto target=uint16_t(Word(source,at+0x334)>>16);
        std::fill(edited.begin()+at,edited.begin()+at+0x2c,0);
        std::copy(q.name.begin(),q.name.end(),edited.begin()+at);
        edited[at+0x8a]=q.stars;
        PutWord(edited,at+0x330,q.capture?0x81:1);
        PutHalf(edited,at+0x336,q.count);
        // The template goal text describes its old count/type. Replace it
        // alongside the objective so both game HUD and launcher stay truthful.
        const std::string goal=(q.capture?"Capture ":"Hunt ")+std::to_string(q.count)+" target monster"+(q.count>1?"s":"")+" (ID "+std::to_string(target)+")";
        std::fill(edited.begin()+at+0x2e,edited.begin()+at+0x88,0);
        std::copy(goal.begin(),goal.end(),edited.begin()+at+0x2e);
        return edited;
    }
    throw std::runtime_error("Quest template no longer exists on this disc");
}
void SaveCustomQuest(const std::filesystem::path& user,const CustomQuest& q) {
    Validate(q);
    if(q.capture && !SupportsCapture(q))throw std::runtime_error("This template's target has no verified capture support");
    // Validate against the actual disc before writing any user record.
    (void)EditQuestFile(GameDiscImage().ReadFile(QuestPath(q.set)),q);
    const auto directory=Directory(user);
    std::filesystem::create_directories(directory);
    std::string key=q.storageKey;
    if(key.empty()) {
        unsigned serial=1;
        do{key=Key(q)+"-"+std::to_string(serial++);}while(std::filesystem::exists(directory/(key+".mh3quest")));
    }
    const auto path=directory/(key+".mh3quest");
    const auto temporary=directory/(key+".writing");
    std::ofstream output(temporary,std::ios::trunc);
    output<<"mh3-custom-quest-v1\n"<<unsigned(q.set)<<' '<<q.base<<' '<<q.count<<' '<<q.capture<<' '<<unsigned(q.stars)<<'\n'<<q.name<<'\n';
    output.close();
    if(!output) throw std::runtime_error("Cannot save custom quest");
    std::filesystem::rename(temporary,path);
}
std::vector<CustomQuest> LoadCustomQuests(const std::filesystem::path& user) {
    std::vector<CustomQuest> result;
    if(!std::filesystem::exists(Directory(user))) return result;
    for(const auto& entry:std::filesystem::directory_iterator(Directory(user))) {
        if(entry.path().extension()!=".mh3quest" || !entry.is_regular_file()) continue;
        std::ifstream input(entry.path()); std::string version; std::getline(input,version);
        CustomQuest q; unsigned set=0,stars=0;
        if(version!="mh3-custom-quest-v1" || !(input>>set>>q.base>>q.count>>q.capture>>stars)) continue;
        input.ignore(1024,'\n'); std::getline(input,q.name);
        if(set>2 || stars>255) continue;
        q.set=QuestCatalog::Set(set);q.stars=stars;
        q.storageKey=entry.path().stem().string();
        try { Validate(q); result.push_back(q); } catch(const std::exception&) { }
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.name<b.name;});
    return result;
}
void ActivateCustomQuest(const CustomQuest& q) {
    if(q.capture && !SupportsCapture(q))throw std::runtime_error("This template's target has no verified capture support");
    auto& disc=GameDiscImage();
    disc.SetFileOverride(QuestPath(q.set),EditQuestFile(disc.ReadFile(QuestPath(q.set)),q));
}
bool SupportsCapture(const CustomQuest& q) {
    const auto entries=[](const std::vector<uint8_t>& bytes) {
        if(bytes.size()<8 || Word(bytes,0)!=0x4f4d4f4d || Word(bytes,4)>(bytes.size()-8)/8)throw std::runtime_error("Invalid quest table");
        std::vector<size_t> offsets;
        for(uint32_t i=0;i<Word(bytes,4);++i) {
            const size_t at=Word(bytes,8+i*8),size=Word(bytes,12+i*8);
            if(size<0x4b8 || at>bytes.size() || size>bytes.size()-at)throw std::runtime_error("Invalid quest extent");
            offsets.push_back(at);
        }
        return offsets;
    };
    const auto templates=GameDiscImage().ReadFile(QuestPath(q.set));uint16_t target=0;
    for(size_t at:entries(templates))if((Word(templates,at+0x2c)>>16)==q.base){target=Word(templates,at+0x334)>>16;break;}
    if(!target)return false;
    // Derive capture support from authored quests, not guessed monster IDs.
    for(const auto set:{QuestCatalog::Set::Village,QuestCatalog::Set::Online,QuestCatalog::Set::Arena})
        {
            const auto bytes=GameDiscImage().ReadFile(QuestPath(set));
            for(size_t at:entries(bytes))for(unsigned i=0;i<3;++i)
                if(Word(bytes,at+0x330+i*8)==0x81 && (Word(bytes,at+0x334+i*8)>>16)==target)return true;
        }
    return false;
}
}
