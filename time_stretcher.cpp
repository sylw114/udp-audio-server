// ============================================================================
// time_stretcher.cpp — 保持音高的轻量级 WSOLA 时间伸缩器
// ============================================================================

#include "time_stretcher.h"

#include <algorithm>
#include <cmath>
#include <cstring>

void TimeStretcher::init(uint32_t sampleRate, uint8_t channels)
{
    sampleRate_ = sampleRate;
    channels_ = channels;

    windowFrames_ = std::max<size_t>(256, sampleRate_ * 30 / 1000);
    overlapFrames_ = std::max<size_t>(64, sampleRate_ * 10 / 1000);
    outputHopFrames_ = windowFrames_ - overlapFrames_;
    seekFrames_ = std::max<size_t>(32, sampleRate_ * 8 / 1000);

    reset();
}

void TimeStretcher::reset()
{
    input_.clear();
    output_.clear();
    prevOverlap_.assign(overlapFrames_ * channels_, 0);
    nextInputFrame_ = 0.0;
    primed_ = false;
}

void TimeStretcher::setSpeed(double speed)
{
    if (!std::isfinite(speed))
        speed = 1.0;
    speed_ = std::clamp(speed, 0.25, 4.0);
}

void TimeStretcher::appendInput(const int16_t* samples, size_t sampleCount)
{
    if (!samples || sampleCount == 0)
        return;
    input_.insert(input_.end(), samples, samples + sampleCount);
}

size_t TimeStretcher::availableOutputSamples()
{
    produceMore();
    return output_.size();
}

size_t TimeStretcher::readOutput(int16_t* dst, size_t sampleCount)
{
    produceMore();
    size_t toRead = std::min(sampleCount, output_.size());
    if (toRead == 0)
        return 0;

    memcpy(dst, output_.data(), toRead * sizeof(int16_t));
    output_.erase(output_.begin(), output_.begin() + toRead);
    return toRead;
}

void TimeStretcher::produceMore()
{
    if (channels_ == 0 || windowFrames_ == 0)
        return;

    const size_t windowSamples = windowFrames_ * channels_;
    const size_t overlapSamples = overlapFrames_ * channels_;
    const size_t outputHopSamples = outputHopFrames_ * channels_;

    while (true)
    {
        size_t inputFrames = input_.size() / channels_;
        if (!primed_)
        {
            if (inputFrames < windowFrames_)
                break;

            output_.insert(output_.end(), input_.begin(), input_.begin() + outputHopSamples);
            prevOverlap_.assign(input_.begin() + outputHopSamples, input_.begin() + windowSamples);
            nextInputFrame_ = speed_ * (double)outputHopFrames_;
            primed_ = true;
            continue;
        }

        size_t expectedFrame = (size_t)std::max(0.0, std::floor(nextInputFrame_));
        size_t neededFrame = expectedFrame + seekFrames_ + windowFrames_;
        if (inputFrames < neededFrame)
            break;

        int bestOffset = findBestOffset(expectedFrame);
        size_t startFrame = expectedFrame + bestOffset;
        size_t startSample = startFrame * channels_;

        for (size_t f = 0; f < overlapFrames_; ++f)
        {
            double t = (double)(f + 1) / (double)(overlapFrames_ + 1);
            size_t frameBase = f * channels_;
            for (uint8_t ch = 0; ch < channels_; ++ch)
            {
                double a = (double)prevOverlap_[frameBase + ch] * (1.0 - t);
                double b = (double)input_[startSample + frameBase + ch] * t;
                output_.push_back(clampToInt16(a + b));
            }
        }

        size_t bodyStart = startSample + overlapSamples;
        size_t bodyEnd = startSample + outputHopSamples;
        output_.insert(output_.end(), input_.begin() + bodyStart, input_.begin() + bodyEnd);

        size_t tailStart = startSample + outputHopSamples;
        prevOverlap_.assign(input_.begin() + tailStart, input_.begin() + tailStart + overlapSamples);

        nextInputFrame_ += speed_ * (double)outputHopFrames_;

        size_t keepFromFrame = 0;
        if (nextInputFrame_ > (double)(seekFrames_ + windowFrames_))
            keepFromFrame = (size_t)(nextInputFrame_ - (double)(seekFrames_ + windowFrames_));
        if (keepFromFrame > 0)
        {
            size_t eraseSamples = std::min(keepFromFrame * channels_, input_.size());
            input_.erase(input_.begin(), input_.begin() + eraseSamples);
            nextInputFrame_ -= (double)(eraseSamples / channels_);
        }
    }
}

int TimeStretcher::findBestOffset(size_t expectedFrame) const
{
    int bestOffset = 0;
    double bestScore = -1.0;
    int seek = (int)seekFrames_;

    for (int offset = -seek; offset <= seek; offset += 4)
    {
        if (offset < 0 && expectedFrame < (size_t)(-offset))
            continue;
        size_t candidate = expectedFrame + offset;
        if ((candidate + windowFrames_) * channels_ > input_.size())
            continue;

        double score = overlapScore(candidate);
        if (score > bestScore)
        {
            bestScore = score;
            bestOffset = offset;
        }
    }

    return bestOffset;
}

double TimeStretcher::overlapScore(size_t candidateFrame) const
{
    double dot = 0.0;
    double a2 = 0.0;
    double b2 = 0.0;
    size_t candidateSample = candidateFrame * channels_;

    for (size_t f = 0; f < overlapFrames_; ++f)
    {
        double a = 0.0;
        double b = 0.0;
        size_t frameBase = f * channels_;
        for (uint8_t ch = 0; ch < channels_; ++ch)
        {
            a += (double)prevOverlap_[frameBase + ch];
            b += (double)input_[candidateSample + frameBase + ch];
        }
        dot += a * b;
        a2 += a * a;
        b2 += b * b;
    }

    if (a2 <= 1.0 || b2 <= 1.0)
        return 0.0;
    return dot / std::sqrt(a2 * b2);
}

int16_t TimeStretcher::clampToInt16(double value)
{
    value = std::clamp(value, -32768.0, 32767.0);
    return (int16_t)std::lrint(value);
}
