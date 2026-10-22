// The hunter's name on the lobby server, RMHP08 0x803FE8F4 (a PatInterface
// method; decompiled with Ghidra), with the Vita's name tag.
//
// When the hunter picks a Capcom ID slot, the online menu (0x8044E4FC) copies
// the save's name (get_userdata + 3) into PatInterface's user object, which
// ReqUserObject then sends to the lobby server (0x804004A8, the fields that
// differ from the server's copy). The server keeps it as the hunter's name and
// sends it to everyone else: the City's player lists, the quest board's
// members, search results, friend lists and chat. The City's player list
// takes the other players' names from the server only, not from them.
//
// OnlineNameTag (online.h) goes in front of the name there, so players on
// other platforms see who plays on a Vita. The save and the hunter's other
// copies of the name stay as they were. A tagged name holds at most 8 bytes:
// the community server (MH3SP's MySQL backend) keeps the name in an
// 8-character column and refuses a longer one with a database error, and the
// hunter cannot go online. In bytes it fits however the server counts
// characters. The name keeps at least 5 bytes of it (all of a shorter one),
// so a longer tag (MH3_ONLINE_NAME_TAG) is cut to what is left; both are cut
// whole characters at a time.
//
// MH3_PORT_CHECK=SetUserObjectName checks the port with no tag.
#include "online.h"

#include "abi_bridge.h"
#include "memory.h"
#include "port_check.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace {

// PatInterface's user object (+0x8B5C, 0x5C bytes): its hunter name field.
constexpr uint32_t kUserObjectName = 0x8B68;
constexpr size_t kUserObjectNameMax = 0x1F;

constexpr size_t kTaggedNameMax = 8;

// The bytes of the name a tag never takes: as many as "PV " leaves.
constexpr size_t kNameKept = 5;

std::string GuestString(uint32_t address) {
    std::string text;

    while (const char c = char(Memory::Read8(address + uint32_t(text.size()))))
        text += c;

    return text;
}

// The whole characters of `text` within `bytes`. A Shift-JIS lead byte and
// the byte after it count as one character.
std::string Fitted(const std::string& text, size_t bytes) {
    size_t end = 0;

    while (end < text.size()) {
        const unsigned char lead = text[end];
        const bool pair = ((lead >= 0x81 && lead <= 0x9F) || (lead >= 0xE0 && lead <= 0xFC)) && end + 1 < text.size();
        const size_t width = pair ? 2 : 1;

        if (end + width > bytes)
            break;

        end += width;
    }

    return text.substr(0, end);
}

// The tag in what the name leaves of the 8 bytes, then the name.
std::string TaggedName(const std::string& name) {
    const std::string tag = OnlineNameTag();

    if (tag.empty() || name.starts_with(tag))
        return name;

    const std::string front = Fitted(tag, kTaggedNameMax - std::min(name.size(), kNameKept));
    return front + Fitted(name, kTaggedNameMax - front.size());
}

// SetUserObjectName(pat, name): at most 31 bytes of the name and a terminator.
void SetUserObjectNamePort(CpuContext* cpu) {
    const uint32_t pat = cpu->gpr[3];
    std::string name = TaggedName(GuestString(cpu->gpr[4]));

    if (name.size() > kUserObjectNameMax)
        name.resize(kUserObjectNameMax);

    for (size_t i = 0; i <= name.size(); ++i)
        Memory::Write8(pat + kUserObjectName + uint32_t(i), i < name.size() ? uint8_t(name[i]) : 0);
}

} // namespace

std::string OnlineNameTag() {
    if (const char* value = std::getenv("MH3_ONLINE_NAME_TAG"))
        return value;

#ifdef __vita__
    return "PV ";
#else
    return "";
#endif
}

extern "C" void func_803FE8F4(CpuContext* cpu);
extern "C" void MH3_SetUserObjectName(CpuContext* cpu) {
    PortCheck::Call(cpu, "SetUserObjectName", func_803FE8F4, SetUserObjectNamePort);
}
REGISTER_NATIVE_FUNCTION_AS(0x803FE8F4, MH3_SetUserObjectName, "SetUserObjectName");
