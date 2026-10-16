// The game's boot, RMHP08: C ports of the boot init (0x80046CE4, from main)
// and the boot task (0x80059648, slot 0), decompiled from the translation.
//
// The boot init sets up the pads, the system's language and sound mode, the
// draw buckets and a new working character (NewUserData, save_data.cpp: its
// seeds and the random streams' from the time base). The boot task then runs
// once a step through its phases (+8):
//   0  starts the 0x800CE704 task, loads the strap screen's textures
//   1  sets every random stream to 0x10, so the time-based seeds never
//      reach a new character: they all start on the cycle holding 2
//   2  fades the strap screen in (+0xC from 15 to 0)
//   3  holds it until a button (after 30 steps) or for 600 steps, the
//      prompt (+0xE) blinking every 150
//   4  fades it out over 20 steps
//   5  checks the save files and creates the missing ones (state +9,
//      sub-state +10, the next data file +11), with the NAND messages
//   6, 7  takes the options from the system file and starts the title.
// GameTasks::SkipStrapScreen (game_tasks.cpp) jumps from 2 to the end of 4.
#include "abi_bridge.h"
#include "guest_call.h"
#include "memory.h"
#include "port_check.h"

#include <algorithm>
#include <cstdint>

namespace {

constexpr uint32_t kSystem = 0x8065AA60; // system_w
enum : uint32_t {
    kLanguage = 0x01,       // u8
    kSoundMode = 0x03,      // u8
    kBootState = 0x22,      // u8: 0 boot, 1-2 running
    kFramePhase = 0x23,     // u8
    kBootBusy = 0x2B,       // u8: 1 while the boot task runs
    kResetDisabled = 0x31,  // u8: 1 = ignore HOME
    kBootFF = 0x7E8,        // two u8 the boot sets to 0xFF
    kSystemFileMade = 0x856, // u8: the system file exists or was made
    kSaveReady = 0x857,     // u8: the save files check out
    kSaveBusy = 0x865,      // u8: a NAND access runs (no reset meanwhile)
    kBootZero = 0x871,      // u8 the boot clears
    kTextureHook = 0x8C4,   // u32: called between the strap screen's texture loads
    kOptionCopy = 0x912,    // 31 bytes: the options the title starts with
    kBootFlag = 0xA58,      // u8: cleared when the strap screen ends
};

constexpr uint32_t kOptions = 0x8065B510; // option_w
constexpr uint32_t kSystemMutex = 0x8066B950;

// Pad 0: the held buttons (u16; 0x3FF0 the remote's), the classic
// controller's (0xFFFC), the pressed ones (0x10: A).
constexpr uint32_t kPadHeld = 0x8065B89E, kPadClassicHeld = 0x8065B8AA, kPadPressed = 0x8065BA94;
constexpr uint16_t kRemoteButtons = 0x3FF0, kClassicButtons = 0xFFFC, kButtonA = 0x10;

constexpr uint32_t kStrapAlpha = 0x807935D3; // u8: the strap screen's fade
constexpr float kAlphaMax = 255.0f, kFadeSteps = 15.0f;

// SDA (r13) words of the save check.
constexpr uint32_t kNandResult = 0x80796D88;  // s32: the last NAND error
constexpr uint32_t kNandMessageClock = 0x80796D90; // u32: (+410) & 0x7FFF a step
constexpr uint32_t kSystemFileFound = 0x80796D8C; // u8: the system file was there
constexpr uint32_t kNextDataFile = 0x80796D8D;   // u8

// The boot task (slot 0).
enum : uint32_t {
    tPhase = 8,      // u8
    tState = 9,      // u8: phase 5's
    tSubState = 10,  // u8
    tDataFile = 11,  // u8: phase 5 state 5's next data file
    tTimer = 12,     // s16
    tBlink = 14,     // u16
    tUnused = 16,    // u16
};

enum : uint32_t {
    OSInitMutex = 0x804D2560,
    BootInitA = 0x800CF448,
    BootInitB = 0x800CF0C0,
    PadInit = 0x80041B08,
    SCGetSoundMode = 0x804DD280,
    SCGetLanguage = 0x804DD150,
    BootInitC = 0x800416E4,
    BootInitD = 0x8005855C,
    InitDrawBuckets = 0x80048834,
    BootInitE = 0x80058094,
    NewUserData = 0x80049818,
    BootInitF = 0x800CFC04, // (u32* 0)
    BootInitG = 0x800D21DC,
    BootInitH = 0x800EEB44,
    BootInitI = 0x8004750C,
    BootInitJ = 0x8044D614,
    BootInitK = 0x80435B40,
    BootInitL = 0x80046F8C,
    BootInitM = 0x8044CFCC,

    BootTaskStart = 0x800CE618,
    StartBootTask = 0x80041854, // (task function, arg)
    BootSideTask = 0x800CE704,
    SetSoftresetFlag = 0x80046F18,
    StrapSetup = 0x8005B2DC,
    StrapSetupB = 0x80041390,
    UITextureTPL = 0x800D0174, // (archive, slot)
    SubTransSet = 0x80049240,  // (function, words, u32* words)
    GetControlType = 0x80043BAC,
    StrapEnd = 0x800CFC54,
    StrapDraw = 0x800595B4,
    NandMessage = 0x8005A3D0,     // (message, argument)
    NandMessageDraw = 0x8005A6AC,
    RandSeedAll = 0x800CEF00,

    // The save manager's steps: the starts return nothing, the polls 1 when
    // done, -1 or -2 on an error (then NandError says which), else 0.
    CheckStart = 0x804413BC,
    CheckPoll = 0x80441460,
    CheckDone = 0x804421D0,
    NandError = 0x80442B54,
    NandErrorMessage = 0x80442B64, // chg_nand_err2msgcode
    SpaceStart = 0x80440078,
    SpacePoll = 0x804400D0,
    SystemFileStart = 0x80440188, // createSystemFile_init
    SystemFilePoll = 0x8044036C,  // createSystemFile
    DataFileStart = 0x80440790,   // createDataFile_init(file, -2, 0)
    DataFilePoll = 0x80440ACC,    // createDataFile(1)
    BannerStart = 0x80441668,
    BannerPoll = 0x80441828,
    FinishStart = 0x80440F98,
    FinishPoll = 0x804410E4,
    SystemFileReadPoll = 0x8044233C,
    SystemFileReadDone = 0x804427FC,
    DataFilesPoll = 0x804428CC,
    RepairStart = 0x804425C4,
    RepairPoll = 0x80442664,
    VISetBlack = 0x804E91A0,
    VIFlush = 0x804E9010,
    ShutdownA = 0x804E8050,
    ShutdownB = 0x804D3070,

    Memcpy = 0x80004000,
    ApplyOptions = 0x803BEE08,
    TitleSetupA = 0x800CFD84,
    TitleSetupB = 0x800D0150,
    TitleSetupC = 0x800527C4,
    TitleSetupD = 0x800419A8,
    EndBootTask = 0x800418B4, // (task)
};

constexpr int32_t kNandNoFile = -4, kNandCorrupt = -15;
constexpr int32_t kNoMessage = -200;

uint8_t Read8(uint32_t address) {
    return Memory::Read8(address);
}

void Write8(uint32_t address, uint8_t value) {
    Memory::Write8(address, value);
}

// A poll's error: -1 or -2.
bool Failed(uint32_t result) {
    return result + 2 <= 1;
}

void BootInit(CpuContext* ctx) {
    GuestFrame frame(ctx, 0x20);
    GuestCall game{ctx};
    game.Call<OSInitMutex>({kSystemMutex});
    game.Call<BootInitA>();
    game.Call<BootInitB>();
    game.Call<PadInit>();
    Write8(kSystem + kSoundMode, uint8_t(game.Call<SCGetSoundMode>()));
    Write8(kSystem + kLanguage, uint8_t(game.Call<SCGetLanguage>()));
    game.Call<BootInitC>();
    game.Call<BootInitD>();
    game.Call<InitDrawBuckets>();
    game.Call<BootInitE>();
    game.Call<NewUserData>();
    Memory::Write32(frame[8], 0);
    game.Call<BootInitF>({frame[8]});
    game.Call<BootInitG>();
    Write8(kSystem + kBootState, 0);
    Write8(kSystem + kFramePhase, 0);
    Write8(kSystem + kBootZero, 0);
    Write8(kSystem + kBootFF, 0xFF);
    Write8(kSystem + kBootFF + 1, 0xFF);
    game.Call<BootInitH>();
    game.Call<BootInitI>();
    game.Call<BootInitJ>();
    game.Call<BootInitK>();
    game.Call<BootInitL>();
    game.Call<BootInitM>();
}

// The strap screen's alpha for fade step `step` (15 dark .. 0 lit).
void SetStrapAlpha(int16_t step) {
    const float alpha = std::clamp(kAlphaMax * (1.0f - float(step) / kFadeSteps), 0.0f, kAlphaMax);
    Write8(kStrapAlpha, uint8_t(int32_t(alpha)));
}

class BootTask {
public:
    BootTask(CpuContext* ctx, uint32_t task) : frame_(ctx, 0x30), game_{ctx}, task_(task) {
        // The original keeps the task at its frame's +8, the first word
        // handed to SubTransSet.
        Memory::Write32(Words(), task);
    }

    void Run() {
        switch (Read8(task_ + tPhase)) {
        case 0:
            Start();
            break;
        case 1:
            game_.Call<RandSeedAll>({0x10});
            NextPhase();
            Write16(tTimer, 15);
            Write16(tBlink, 0);
            Write16(tUnused, 0);
            break;
        case 2:
            FadeIn();
            break;
        case 3:
            Hold();
            break;
        case 4:
            FadeOut();
            break;
        case 5:
            SaveCheck();
            break;
        case 6:
            game_.Call<Memcpy>({kSystem + kOptionCopy, kOptions, 31});
            Write8(task_ + tPhase, 7);
            break;
        case 7:
            StartTitle();
            break;
        default:
            break;
        }
    }

private:
    GuestFrame frame_;
    GuestCall game_;
    uint32_t task_;
    uint16_t pressed_ = 0;

    uint32_t Words() const {
        return frame_[8];
    }

    void SetWords(uint32_t first) {
        Memory::Write32(Words(), first);
    }

    void SetWords(uint32_t first, uint32_t second) {
        Memory::Write32(Words(), first);
        Memory::Write32(Words() + 4, second);
    }

    int16_t Timer() const {
        return int16_t(Memory::Read16(task_ + tTimer));
    }

    void Write16(uint32_t field, uint16_t value) {
        Memory::Write16(task_ + field, value);
    }

    void NextPhase() {
        Write8(task_ + tPhase, uint8_t(Read8(task_ + tPhase) + 1));
    }

    void SetState(uint8_t state) {
        Write8(task_ + tState, state);
        Write8(task_ + tSubState, 0);
    }

    void NextState() {
        Write8(task_ + tState, uint8_t(Read8(task_ + tState) + 1));
        Write8(task_ + tSubState, 0);
    }

    void NextSubState(uint8_t by = 1) {
        Write8(task_ + tSubState, uint8_t(Read8(task_ + tSubState) + by));
    }

    void SaveBusy(uint8_t busy) {
        Write8(kSystem + kSaveBusy, busy);
    }

    void Start() {
        game_.Call<BootTaskStart>();
        game_.Call<StartBootTask>({BootSideTask, 15});
        NextPhase();
        Write8(task_ + tState, 0);
        Write8(task_ + tSubState, 0);
        game_.Call<SetSoftresetFlag>({0});
        Write8(kSystem + kBootBusy, 1);
        Write8(kSystem + kResetDisabled, 1);
        game_.Call<StrapSetup>();
        game_.Call<StrapSetupB>();
        game_.Call<UITextureTPL>({0x90F49000, 5});
        const uint32_t loaded = game_.Call<UITextureTPL>({0x90001000, 255});
        game_(Memory::Read32(kSystem + kTextureHook), {loaded});
        game_.Call<UITextureTPL>({0x90001660, 253});
    }

    void FadeIn() {
        SetStrapAlpha(Timer());
        game_.Call<SubTransSet>({StrapDraw, 1, Words()});
        const int16_t timer = int16_t(Timer() - 1);
        Write16(tTimer, uint16_t(timer));

        if (timer == 0)
            NextPhase();
    }

    void Hold() {
        game_.Call<SubTransSet>({StrapDraw, 1, Words()});

        if (Timer() > 30) {
            const uint16_t remote = Memory::Read16(kPadHeld) & kRemoteButtons;
            uint16_t classic = 0;

            if (uint8_t(game_.Call<GetControlType>({0})) == 2)
                classic = Memory::Read16(kPadClassicHeld) & kClassicButtons;

            if (remote || classic || Timer() > 600) {
                NextPhase();
                Write16(tTimer, 0);
                return;
            }
        }

        const int16_t timer = int16_t(Timer() + 1);
        Write16(tTimer, uint16_t(timer));

        if (timer < 600 && timer % 150 == 0)
            Write16(tBlink, Memory::Read16(task_ + tBlink) ^ 1);
    }

    void FadeOut() {
        SetStrapAlpha(Timer());
        game_.Call<SubTransSet>({StrapDraw, 1, Words()});
        const int16_t timer = int16_t(Timer() + 1);
        Write16(tTimer, uint16_t(timer));

        if (timer > 20) {
            Write8(kSystem + kResetDisabled, 0);
            Write8(kSystem + kBootFlag, 0);
            game_.Call<StrapEnd>();
            NextPhase();
            Write8(task_ + tState, 0);
        }
    }

    void StartTitle() {
        game_.Call<ApplyOptions>({kSystem + kOptionCopy});
        Write8(kSystem + kBootBusy, 0);
        Write8(kSystem + kBootState, 2);
        game_.Call<TitleSetupA>();
        game_.Call<SetSoftresetFlag>({1});
        game_.Call<TitleSetupB>();
        game_.Call<TitleSetupC>();
        game_.Call<TitleSetupD>();
        game_.Call<EndBootTask>({task_});
    }

    // The NAND message window off: message -200 in one word.
    void NoMessage() {
        game_.Call<NandMessage>({0, 0});
        Memory::Write32(Words(), uint32_t(kNoMessage));
        game_.Call<SubTransSet>({NandMessageDraw, 1, Words()});
    }

    void NoMessageClearSecond() {
        game_.Call<NandMessage>({0, 0});
        SetWords(uint32_t(kNoMessage), 0);
        game_.Call<SubTransSet>({NandMessageDraw, 1, Words()});
    }

    // Phase 5: the save files. Each state's step returns false when it shut
    // the console down, which skips the message at the end.
    void SaveCheck() {
        pressed_ = Memory::Read16(kPadPressed);
        Memory::Write32(kNandMessageClock, (Memory::Read32(kNandMessageClock) + 410) & 0x7FFF);
        bool message = true;

        switch (Read8(task_ + tState)) {
        case 0:
            SaveBusy(1);
            Memory::Write32(kNandResult, 0);
            game_.Call<CheckStart>();
            SetState(1);
            Write16(tBlink, 0);
            break;
        case 1:
            message = CheckFiles();
            break;
        case 2:
            message = WaitNand();
            break;
        case 3:
            message = CreateFiles();
            break;
        case 4:
            message = ReadSystemFile();
            break;
        case 5:
            message = CreateDataFiles();
            break;
        case 6:
            message = Repair();
            break;
        case 7:
            Shutdown();
            return;
        default:
            break;
        }

        if (!message)
            return;

        const uint8_t state = Read8(task_ + tState);

        if ((state == 2 && Memory::Read32(kNandResult) != 0) || state == 6) {
            game_.Call<NandMessage>({Memory::Read32(Words()), Memory::Read32(Words() + 4)});
            game_.Call<SubTransSet>({NandMessageDraw, 2, Words()});
        }
    }

    // State 1: the check's result.
    bool CheckFiles() {
        Memory::Write32(kNandResult, 0);
        Memory::Write32(kNandMessageClock, 0);
        const uint32_t result = game_.Call<CheckPoll>();

        if (!Failed(result)) {
            if (result == 1) {
                SaveBusy(1);
                SetState(4);
                Write16(tTimer, 30);
                Write8(kSystem + kSaveReady, 0);
                game_.Call<CheckDone>();
            }

            NoMessage();
            return true;
        }

        SaveBusy(0);
        const int32_t error = int32_t(game_.Call<NandError>());
        Memory::Write32(kNandResult, uint32_t(error));

        if (error != kNandNoFile)
            return Fail();

        SetState(3);
        NoMessage();
        return true;
    }

    // An error in a NAND step: on to state 2, which runs at once.
    bool Fail() {
        SaveBusy(0);
        Write16(tTimer, 30);
        SetState(2);
        Memory::Write32(kNandMessageClock, 0);
        return WaitNand();
    }

    // State 2: shows the NAND's message. A gives up on a missing or broken
    // file (state 6) or, on messages 16 and 17, shuts the console down.
    bool WaitNand() {
        if (Timer() > 0)
            Write16(tTimer, uint16_t(Timer() - 1));

        const int32_t error = int32_t(game_.Call<NandError>());
        Memory::Write32(kNandResult, uint32_t(error));

        if (error == kNandNoFile || error == kNandCorrupt) {
            SetWords(1, 0);

            if (pressed_ & kButtonA) {
                SetState(6);
                SetWords(7, 0);
            }

            return true;
        }

        const uint32_t message = game_.Call<NandErrorMessage>();
        SetWords(message, 0);

        if (message - 16 <= 1 && (pressed_ & kButtonA)) {
            Shutdown();
            return false;
        }

        return true;
    }

    // State 3: no save files yet: room, the system file, the six data
    // files, the banner, the end.
    bool CreateFiles() {
        switch (Read8(task_ + tSubState)) {
        case 0:
            game_.Call<SpaceStart>();
            SaveBusy(1);
            NextSubState();
            break;
        case 1:
            if (const uint32_t result = game_.Call<SpacePoll>(); Failed(result))
                return Fail();
            else if (result == 1)
                NextSubState();

            break;
        case 2:
            game_.Call<SystemFileStart>();
            SaveBusy(1);
            NextSubState();
            break;
        case 3:
            Memory::Write32(kNandResult, 0);

            if (const uint32_t result = game_.Call<SystemFilePoll>(); Failed(result)) {
                return Fail();
            } else if (result == 1) {
                Write8(kSystem + kSystemFileMade, 1);
                Write8(kNextDataFile, 0);
                NextSubState();
            }

            break;
        case 4:
            game_.Call<DataFileStart>({Read8(kNextDataFile), uint32_t(-2), 0});
            SaveBusy(1);
            NextSubState();
            break;
        case 5:
            Memory::Write32(kNandResult, 0);

            if (const uint32_t result = game_.Call<DataFilePoll>({1}); Failed(result)) {
                return Fail();
            } else if (result == 1) {
                const uint8_t next = uint8_t(Read8(kNextDataFile) + 1);
                Write8(kNextDataFile, next);
                Write8(task_ + tSubState, uint8_t(Read8(task_ + tSubState) + (next == 6 ? 1 : -1)));
            }

            break;
        case 6:
            game_.Call<BannerStart>();
            SaveBusy(1);
            NextSubState();
            break;
        case 7:
            Memory::Write32(kNandResult, 0);

            if (const uint32_t result = game_.Call<BannerPoll>(); Failed(result)) {
                return Fail();
            } else if (result == 1) {
                Write8(kNextDataFile, 0);
                NextSubState();
            }

            break;
        case 8:
            game_.Call<FinishStart>();
            SaveBusy(1);
            NextSubState();
            break;
        case 9:
            Memory::Write32(kNandResult, 0);

            if (const uint32_t result = game_.Call<FinishPoll>(); Failed(result)) {
                return Fail();
            } else if (result == 1) {
                SaveBusy(0);
                NextSubState();
                Write8(kSystem + kSaveReady, 1);
                Write16(tTimer, 60);
            }

            break;
        case 10:
            if (Timer() > 0) {
                Write16(tTimer, uint16_t(Timer() - 1));
            } else {
                Write8(task_ + tPhase, 6);
                Write8(kSystem + kSystemFileMade, 1);
            }

            break;
        default:
            break;
        }

        const uint8_t sub = Read8(task_ + tSubState);

        if (sub <= 1) {
            NoMessage();
            return true;
        }

        // 5 while creating (sub-states 2..9), 6 after.
        const uint32_t message = uint8_t(sub - 2) < 8 ? 5 : 6;
        SetWords(message, 0);

        if (Read8(kSystemFileFound) == 0) {
            game_.Call<NandMessage>({message, 0});
            game_.Call<SubTransSet>({NandMessageDraw, 2, Words()});
        }

        return true;
    }

    // State 4: the save files exist: the system file, then the data files.
    bool ReadSystemFile() {
        Memory::Write32(kNandResult, 0);
        Memory::Write32(kNandMessageClock, 0);

        switch (Read8(task_ + tSubState)) {
        case 0:
            if (const uint32_t result = game_.Call<SystemFileReadPoll>(); Failed(result)) {
                SaveBusy(0);

                if (int32_t(game_.Call<NandError>()) != kNandNoFile) {
                    Write16(tBlink, 1);
                    return Fail();
                }

                NextSubState();
                Write16(tTimer, 30);
                game_.Call<SystemFileStart>();
                SaveBusy(1);
            } else if (result == 1) {
                Write8(kSystemFileFound, 1);
                NextSubState(2);
                Write16(tTimer, 30);
                game_.Call<SystemFileReadDone>();
                Write8(kSystem + kSystemFileMade, 1);
                SaveBusy(1);
            }

            break;
        case 1:
            if (const uint32_t result = game_.Call<SystemFilePoll>(); Failed(result)) {
                return Fail();
            } else if (result == 1) {
                Write8(kSystem + kSystemFileMade, 1);
                SaveBusy(1);
                NextSubState();
                Write16(tTimer, 30);
                game_.Call<SystemFileReadDone>();
            }

            break;
        case 2:
            if (const uint32_t result = game_.Call<DataFilesPoll>(); Failed(result)) {
                SaveBusy(0);
                const int32_t error = int32_t(game_.Call<NandError>());

                if (error != kNandNoFile && error != kNandCorrupt) {
                    Write16(tBlink, 1);
                    return Fail();
                }

                NextSubState();
                Write16(tTimer, 30);
                game_.Call<BannerStart>();
                SaveBusy(1);
            } else if (result == 1) {
                SaveBusy(0);
                NextState();
                Write16(tTimer, 30);
            }

            break;
        case 3:
            if (const uint32_t result = game_.Call<BannerPoll>(); Failed(result)) {
                return Fail();
            } else if (result == 1) {
                SaveBusy(0);
                NextState();
                Write8(task_ + tDataFile, 0);
                Write16(tTimer, 30);
            }

            break;
        default:
            break;
        }

        NoMessageClearSecond();
        return true;
    }

    // State 5: the data files, 0 to 5, each made again where it is missing
    // or broken.
    bool CreateDataFiles() {
        Memory::Write32(kNandResult, 0);
        Memory::Write32(kNandMessageClock, 0);

        switch (Read8(task_ + tSubState)) {
        case 0:
            game_.Call<DataFileStart>({Read8(task_ + tDataFile), uint32_t(-2), 0});
            SaveBusy(1);
            NextSubState();
            break;
        case 1: {
            const uint32_t result = game_.Call<DataFilePoll>({1});

            if (Failed(result)) {
                const int32_t error = int32_t(game_.Call<NandError>());

                if (error != kNandCorrupt && error != kNandNoFile) {
                    Write16(tBlink, 1);
                    return Fail();
                }
            } else if (result != 1) {
                break;
            }

            if (const uint8_t file = Read8(task_ + tDataFile); file < 5) {
                Write8(task_ + tDataFile, uint8_t(file + 1));
                game_.Call<DataFileStart>({uint8_t(file + 1), uint32_t(-2), 0});
            } else {
                SaveBusy(0);
                NextPhase();
                Write8(task_ + tState, 0);
                Write8(task_ + tSubState, 0);
                Write8(task_ + tDataFile, 0);
            }

            break;
        }
        default:
            break;
        }

        NoMessageClearSecond();
        return true;
    }

    // State 6: after A on a missing or broken file: message 7 while the
    // files are made again, then the check from the start.
    bool Repair() {
        switch (Read8(task_ + tSubState)) {
        case 0:
            Memory::Write32(Words(), 7);
            game_.Call<RepairStart>();
            NextSubState();
            break;
        case 1: {
            Memory::Write32(Words(), 7);
            const uint32_t result = game_.Call<RepairPoll>();

            if (Failed(result))
                return Fail();

            if (result == 1) {
                Write8(kSystem + kSaveReady, 0);
                Write16(tTimer, 60);
                NextSubState();
            }

            break;
        }
        case 2:
            Memory::Write32(Words(), 2);

            if (Timer() > 0) {
                Write16(tTimer, uint16_t(Timer() - 1));
            } else {
                game_.Call<CheckStart>();
                SetState(1);
                Write16(tBlink, 0);
                SetWords(3, 0);
            }

            break;
        default:
            break;
        }

        Memory::Write32(Words() + 4, 0);
        return true;
    }

    // State 7, or A on NAND messages 16 and 17: black screen and off.
    void Shutdown() {
        game_.Call<VISetBlack>({1});
        game_.Call<VIFlush>();
        game_.Call<ShutdownA>();
        game_.Call<ShutdownB>();
    }
};

} // namespace

// 0x80046CE4 BootInit(), once from main.
extern "C" void func_80046CE4(CpuContext* ctx);
static void BootInitPort(CpuContext* ctx) {
    BootInit(ctx);
}

extern "C" void MH3_BootInit(CpuContext* ctx) {
    PortCheck::Call(ctx, "BootInit", func_80046CE4, BootInitPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80046CE4, MH3_BootInit, "BootInit");

// 0x80059648 BootTask(task).
extern "C" void func_80059648(CpuContext* ctx);
static void BootTaskPort(CpuContext* ctx) {
    BootTask(ctx, ctx->gpr[3]).Run();
}

extern "C" void MH3_BootTask(CpuContext* ctx) {
    PortCheck::Call(ctx, "BootTask", func_80059648, BootTaskPort);
}
REGISTER_NATIVE_FUNCTION_AS(0x80059648, MH3_BootTask, "BootTask");
