#include "vita_launcher.h"
#include "disc_image.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace {
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Word(std::vector<uint8_t>& b,size_t at,uint32_t v){for(unsigned i=0;i<4;++i)b.at(at+i)=v>>(24-i*8);}
void Save(const std::filesystem::path& p,const std::vector<uint8_t>& b){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());if(!f)throw std::runtime_error("test fixture write");}
std::vector<uint8_t> Quest() {
    std::vector<uint8_t> q(16+0x4c0);Word(q,0,0x4f4d4f4d);Word(q,4,1);Word(q,8,16);Word(q,12,0x4c0);
    Word(q,16+0x2c,1060u<<16);Word(q,16+0x330,1);Word(q,16+0x334,(20u<<16)|1);q[16+0x8a]=2;q[16+0x8b]=1;
    return q;
}
void SaveTests(const std::filesystem::path& root) {
    std::vector<uint8_t> file(0x14000);Word(file,0,1);Word(file,0x48+0x6000,1);Word(file,4,1);
    Check(!VitaLauncher::ValidSave(file,0),"empty character must be rejected");
    Check(VitaLauncher::ValidSave(file,1),"populated second character must be found");
    auto dir=root/"nand/title/00010000/524d4850/data/nocopy";std::filesystem::create_directories(dir);Save(dir/"data04",file);
    const auto slots=VitaLauncher::ProbeSaves(root);Check(slots.size()==1 && slots[0].file==4 && slots[0].character==1,"real MH3 slot probe");
    file.back()=1;Check(!VitaLauncher::ValidSave(file,1),"bad checksum must be rejected");
    file.resize(12);Check(!VitaLauncher::ValidSave(file,1),"short file must be rejected");
    Check(std::filesystem::file_size(dir/"data04")==0x14000,"probe must not change the save");
}
void QuestTests() {
    const auto original=Quest();VitaLauncher::CustomQuest custom{"Own quest",QuestCatalog::Set::Village,1060,1,true,7};
    const auto edited=VitaLauncher::EditQuestFile(original,custom);
    Check(edited.size()==original.size() && std::string(edited.begin()+16,edited.begin()+25)=="Own quest","quest name/extent");
    Check(edited[16+0x333]==0x81 && edited[16+0x337]==1 && edited[16+0x8a]==7,"actual objective changes");
    Check(edited[16+0x8b]==original[16+0x8b] && edited[16+0x335]==20,"template map/monster retained");
    custom.count=3;bool impossible=false;try{VitaLauncher::EditQuestFile(original,custom);}catch(const std::exception&){impossible=true;}Check(impossible,"unsupported additional spawns rejected");
    auto bad=original;Word(bad,8,0xfffffff0);bool caught=false;
    try{VitaLauncher::EditQuestFile(bad,custom);}catch(const std::exception&){caught=true;}Check(caught,"bad quest extent rejected");
}
void DiscTests(const std::filesystem::path& root) {
    const auto disc=root/"disc";std::filesystem::create_directories(disc/"sys");
    std::vector<uint8_t> boot(0x440);std::copy_n("RMHP08",6,boot.begin());Word(boot,0x18,0x5d1c9ea3);Word(boot,0x420,0x3000/4);Word(boot,0x424,0x3400/4);
    const auto quest=Quest();
    // Root/05/quest00_eng.bin and its raw DVD byte range.
    std::vector<uint8_t> fst(36+20);Word(fst,0,0x01000000);Word(fst,8,3);Word(fst,12,0x01000000);Word(fst,20,3);Word(fst,24,3);Word(fst,28,0x4000/4);Word(fst,32,quest.size());
    std::copy_n("05\0quest00_eng.bin\0",19,fst.begin()+36);Word(boot,0x428,fst.size()/4);
    // FST is aligned to its boot header's size units.
    fst.resize((fst.size()+3)&~size_t(3));Word(boot,0x428,fst.size()/4);
    std::vector<uint8_t> partition(0x7c00);std::copy(boot.begin(),boot.end(),partition.begin());std::copy(fst.begin(),fst.end(),partition.begin()+0x3400);std::copy(quest.begin(),quest.end(),partition.begin()+0x4000);
    Save(disc/"sys/boot.bin",boot);Save(disc/"sys/bi2.bin",std::vector<uint8_t>(0x2000));Save(disc/"sys/apploader.img",std::vector<uint8_t>(0x20));Save(disc/"sys/main.dol",std::vector<uint8_t>(0x100));Save(disc/"sys/fst.bin",fst);Save(disc/"partition.bin",partition);
    {std::ofstream layout(disc/"disc-layout.txt");layout<<"mh3-partition-v1\n";}
    DiscImage image(disc);const auto edited=VitaLauncher::EditQuestFile(quest,{"Own quest",QuestCatalog::Set::Village,1060,1,false,3});
    image.SetFileOverride("05/quest00_eng.bin",edited);
    Check(image.Read(0x4000+16,9)==std::vector<uint8_t>(edited.begin()+16,edited.begin()+25),"partial raw DVD read must see custom quest");
    Check(image.ReadFile("05/quest00_eng.bin")==edited,"full quest read override");
    bool caught=false;try{image.SetFileOverride("05/quest00_eng.bin",{0});}catch(const std::exception&){caught=true;}Check(caught,"resized overlay rejected");
    // A real decrypted ISO sector and partition metadata, processed through
    // the production bounded reader/extractor. Empty texture inventory is valid.
    constexpr size_t partitionAt=0x50000,dataAt=0x70000;
    std::vector<uint8_t> iso(dataAt+0x8000);std::copy(boot.begin(),boot.begin()+0x100,iso.begin());Word(iso,0x40000,1);Word(iso,0x40004,0x40020/4);Word(iso,0x40020,partitionAt/4);
    Word(iso,partitionAt+0x2b8,(dataAt-partitionAt)/4);Word(iso,partitionAt+0x2bc,0x8000/4);Word(iso,partitionAt+0x2b4,0x2c0/4);
    std::copy(partition.begin(),partition.end(),iso.begin()+dataAt+0x400);Save(root/"game.iso",iso);
    unsigned updates=0;VitaLauncher::ProcessDisc(root/"game.iso",root/"processed",[&](const auto&,uint64_t,uint64_t){++updates;});
    Check(updates>1 && VitaLauncher::DiscReady(root/"processed"),"processing publishes validated game data");
    Check(std::filesystem::file_size(root/"game.iso")==iso.size(),"ROM remains untouched");
    DiscImage processed(root/"processed");Check(processed.ReadFile("05/quest00_eng.bin")==quest,"processed partition exact bytes");
    std::filesystem::resize_file(root/"processed/native-textures.bin",12);Check(!VitaLauncher::DiscReady(root/"processed"),"partial pack must not be ready");
}
}
int main(int argc,char** argv) {
    try {
        if(argc==4 && std::string_view(argv[1])=="--process") {
            std::string last;
            VitaLauncher::ProcessDisc(argv[2],argv[3],[&](const std::string& phase,uint64_t done,uint64_t total){
                if(phase!=last){std::cout<<phase<<" ("<<total<<")\n";last=phase;}
            });
            Check(VitaLauncher::DiscReady(argv[3]),"processed external disc readiness");return 0;
        }
        if(argc!=2)throw std::runtime_error("temporary directory argument required");
        const std::filesystem::path root=std::filesystem::path(argv[1])/("launcher-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));std::filesystem::create_directories(root);
        SaveTests(root/"user");QuestTests();DiscTests(root);
        std::filesystem::remove_all(root);
        std::cout<<"MH3 launcher save, quest overlay and ISO processing tests passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
