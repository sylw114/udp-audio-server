#pragma once
// ============================================================================
// time_stretcher.h — 保持音高的轻量级 WSOLA 时间伸缩器
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <vector>

class TimeStretcher {
public:
    void init(uint32_t sampleRate, uint8_t channels);
    void reset();
    void setSpeed(double speed);
    double speed() const { return speed_; }

    void appendInput(const int16_t* samples, size_t sampleCount);
    size_t availableOutputSamples();
    size_t readOutput(int16_t* dst, size_t sampleCount);
    size_t inputQueuedSamples() const { return input_.size(); }

private:
    void produceMore();
    int findBestOffset(size_t expectedFrame) const;
    double overlapScore(size_t candidateFrame) const;
    static int16_t clampToInt16(double value);

    uint32_t sampleRate_ = 0;
    uint8_t channels_ = 0;
    double speed_ = 1.0;

    size_t windowFrames_ = 0;
    size_t overlapFrames_ = 0;
    size_t outputHopFrames_ = 0;
    size_t seekFrames_ = 0;

    std::vector<int16_t> input_;
    std::vector<int16_t> output_;
    std::vector<int16_t> prevOverlap_;
    double nextInputFrame_ = 0.0;
    bool primed_ = false;
};
