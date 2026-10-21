#pragma once

// The variable step's key hold of the NW4R float keys (res_anm_keys.cpp).
namespace ResAnmKeys {

// While an animation object evaluates its keys: the frames it advances a
// 1/30 s tick (its FrameCtrl rate, at least 1) and the frame on that grid
// that the fixed step shows now. The fixed step samples it only every that
// many frames, so between keys at most that far apart a variable step shows
// the value at that frame. Restores the previous ones when it goes.
class HoldSpan {
  public:
    HoldSpan(float frames, float atTick);
    ~HoldSpan();
    HoldSpan(const HoldSpan&) = delete;
    HoldSpan& operator=(const HoldSpan&) = delete;

  private:
    float previous_, previousTick_;
};

} // namespace ResAnmKeys
