#pragma once
#include <cstdint>
/* Model instances drawn on the render thread (model_instance.cpp). The game
 * thread captures an eligible scene draw into the frame packet: the owner's
 * scene task is the native ModelSceneTask and it has no owner callback. The
 * render thread draws it in packet order under a view of the guest memory
 * captured with it, against its replica of the host state (draw_replica.h). */
bool NativeModelInstancesEnabled();
/* Render thread lifecycle (native_frame_packet.cpp), on the game thread:
 * start seeds the render replica and mirrors mutations to it from then on. */
void StartNativeModelInstances();
void StopNativeModelInstances();
/* Game thread, at the scene draw (8004978C): true when the draw was captured
 * instead of performed. *learn is set when the game thread must perform it
 * under a NativeModelInstanceLearning first: the owner's reads are not known
 * yet, so a capture would leave the render thread reading live memory. */
bool CaptureNativeModelInstance(uint32_t owner, uint32_t options, bool* learn);
/* Game thread: while alive, model draws read live memory through a recording
 * view and the lines they read become the owner's captured lines. */
class NativeModelInstanceLearning {
public:
    explicit NativeModelInstanceLearning(uint32_t owner);
    ~NativeModelInstanceLearning();
    NativeModelInstanceLearning(const NativeModelInstanceLearning&) = delete;
    NativeModelInstanceLearning& operator=(const NativeModelInstanceLearning&) = delete;

private:
    uint32_t owner;
};
/* Render thread: draws instance index of the packet in slot. */
void ExecuteNativeModelInstance(uint32_t slot, uint32_t index);
/* model_draw_dispatch.cpp: the scene draw of one instance under its view.
 * scratch holds the words the draw passes by reference (0x60 bytes). */
void DrawNativeModelInstance(uint32_t owner, uint32_t options, const uint32_t sources[3], uint32_t scratch);
/* model_scope.cpp: begins an instance's items; on the render thread each
 * prepared item is drawn at once. */
void BeginNativeModelInstance();
/* Draws the instance's prepared items, each under its preparation state. */
void DrawNativeModelInstanceItems();
/* Development validation (MH3_MODEL_INSTANCES=2): the game thread draws every
 * instance itself and notes each prepared item's drawing state; the render
 * thread prepares the captured instance, compares instead of drawing, and
 * reports the first differing state component. */
void FinishNativeModelInstance();
/* model_scope.cpp, after an item was prepared: true when validation took it
 * (the render thread must not draw it). */
bool NoteNativeModelInstanceItem(const void* prepared);
