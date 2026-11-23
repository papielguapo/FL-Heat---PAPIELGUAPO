#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "OcelotLUT.h"
#include <cmath>
#include <array>
#include <algorithm>
#include <atomic>
#include <memory>

namespace flheat
{

/**
 * Exact 1:1 Decompiled Dave Hill / Crane Song Ocelot (Pro Tools HEAT) Algorithm.
 *
 * Reverse-engineered directly from HEAT.aaxplugin (Ocelot.pdb):
 * - Function 0x500003290: Exact 48-byte coefficient packet generator (`OcelotCoefPacket`)
 * - Function 0x500001B70: Exact per-channel DSP engine (`AlgEntry`)
 * - Table    0x5000194B0: Exact 32,768-float Dual Waveshaper LUT (`kOcelotWaveshaperLUT`)
 *     * Bank 0 [0..16383]: Negative Drive (Odd-Harmonic Console / Solid-State Curve)
 *     * Bank 1 [16384..32767]: Positive Drive (Even-Harmonic Triode / Tape Asymmetric Curve)
 *
 * Oversampling Modes (selectable via Right-Click menu):
 * - 0: 1x Native (0 samples latency — 100% bit-exact to original Pro Tools HEAT AlgEntry!)
 * - 1: 2x Oversampling (Polyphase IIR Half-band)
 * - 2: 4x Oversampling (Polyphase IIR Half-band)
 * - 3: 8x Oversampling (Polyphase IIR Half-band)
 */
struct OcelotCoefPacket
{
    int   tableOffset  = 0;          // +0x00: 0 (Negative Drive Bank 0) or 16384 (Positive Drive Bank 1)
    float preCoef1     = 0.625f;     // +0x04: High-frequency harmonic excitation feed
    float preDiffCoef  = 0.7011719f; // +0x08: Differentiator feedback coefficient (1 - c08)
    float preScaleCoef = 1.453125f;  // +0x0C: Differentiator input scale
    float normScale    = 0.0f;       // +0x10: Sample-rate shift normalization
    float postMix1     = 0.0f;       // +0x14: Post-filter feedback coefficient
    float driveDepth   = 0.0f;       // +0x18: Nonlinear harmonic injection depth
    float dryMakeUp    = 1.0f;       // +0x1C: Output level makeup scalar
    float postB0       = 0.0f;       // +0x20: Tone post-filter b0
    float postA1       = 1.0f;       // +0x24: Tone post-filter a1 (1 - c24)
    float postG        = 0.0f;       // +0x28: Tone shelf recombination gain
    int   bypassFlag   = 0;          // +0x2C: 1 if bypassed or Drive == 0
};

class HeatDSP
{
public:
    enum OversamplingMode
    {
        kOS_1x_NativePT = 0, // 1x (0 latency, bit-exact to Pro Tools HEAT native)
        kOS_2x          = 1, // 2x
        kOS_4x          = 2, // 4x (default HQ)
        kOS_8x          = 3  // 8x (ultra mastering)
    };

    HeatDSP()
    {
        oversamplers[0] = std::make_unique<juce::dsp::Oversampling<float>> (
            2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true); // 2x
        oversamplers[1] = std::make_unique<juce::dsp::Oversampling<float>> (
            2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true); // 4x
        oversamplers[2] = std::make_unique<juce::dsp::Oversampling<float>> (
            2, 3, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true); // 8x
    }

    void prepare (double sampleRate, int maxBlockSize)
    {
        baseSampleRate = (sampleRate > 1000.0) ? sampleRate : 44100.0;
        const size_t blockSz = static_cast<size_t> (std::max (1, maxBlockSize));

        for (auto& os : oversamplers)
        {
            os->reset();
            os->initProcessing (blockSz);
        }

        updateRatesForMode (currentOSMode);

        bypassMixSmoothed.reset (baseSampleRate, 0.010);
        bypassMixSmoothed.setCurrentAndTargetValue (1.0f);

        dryBuffer.setSize (2, std::max (1, maxBlockSize), false, true, false);

        reset();

        harmonicActivity.store (0.0f, std::memory_order_relaxed);
        harmonicPolarity.store (0.0f, std::memory_order_relaxed);
    }

    void reset() noexcept
    {
        for (auto& os : oversamplers)
            if (os != nullptr)
                os->reset();

        for (int ch = 0; ch < 2; ++ch)
        {
            stateZ0[ch] = 0.0f;
            stateZ1[ch] = 0.0f;
            dcX1[ch]    = 0.0f;
            dcY1[ch]    = 0.0f;
        }
    }

    int getLatencySamples (int osMode) const noexcept
    {
        const int mode = juce::jlimit (0, 3, osMode);
        if (mode == kOS_1x_NativePT)
            return 0;

        const auto& os = oversamplers[static_cast<size_t> (mode - 1)];
        return (os != nullptr) ? static_cast<int> (std::lround (os->getLatencyInSamples())) : 0;
    }

    void setParameters (float driveValue, float toneValue, bool isBypassed, int osMode) noexcept
    {
        const int clampedOS = juce::jlimit (0, 3, osMode);
        if (clampedOS != currentOSMode)
        {
            currentOSMode = clampedOS;
            updateRatesForMode (currentOSMode);
            for (int ch = 0; ch < 2; ++ch)
            {
                stateZ0[ch] = 0.0f;
                stateZ1[ch] = 0.0f;
            }
        }

        targetDrive  = juce::jlimit (-10.0f, 10.0f, driveValue);
        targetTone   = juce::jlimit (-10.0f, 10.0f, toneValue);
        targetBypass = isBypassed;

        driveSmoothed.setTargetValue (targetDrive);
        toneSmoothed.setTargetValue (targetTone);
        bypassMixSmoothed.setTargetValue (targetBypass ? 0.0f : 1.0f);
    }

    /**
     * Exact decompiled coefficient generator from HEAT.aaxplugin @ 0x500003290.
     * Maps Drive [-10..+10] and Tone [-10..+10] into the 48-byte Ocelot coefficient packet.
     */
    static inline OcelotCoefPacket computeOcelotCoefficients (float driveUI,
                                                              float toneUI,
                                                              double effectiveSampleRate,
                                                              bool bypassed) noexcept
    {
        OcelotCoefPacket c;

        // Map UI Drive [-10..+10] to Pro Tools internal Ocelot range [-1.0..+1.0]
        double xmm9 = static_cast<double> (driveUI) * 0.1;

        // Map UI Tone [-10..+10] to Pro Tools internal Ocelot Tone [0.0..6.0] (center = 3.0)
        double xmm8 = (static_cast<double> (toneUI) + 10.0) * 0.3;

        // Note: In 0x5000033E6..0x500003460, Pro Tools clamps the internal Ocelot sample-rate ratio
        // to [48000, 96000, 192000] (0xBB80, 0x17700, 0x2EE00) so the differentiator corner frequency
        // remains stable even at high oversampled rates!
        const double clampedRate = juce::jlimit (44100.0, 192000.0, effectiveSampleRate);
        double xmm10 = 44100.0 / clampedRate;

        // Exact Tone piecewise scaling from 0x500003462..0x500003517
        if (xmm8 >= 1.5 && xmm8 < 2.5)
            xmm8 *= 1.06;
        else if (xmm8 >= 2.5 && xmm8 < 3.5)
            xmm8 *= 1.12;
        else if (xmm8 >= 3.5 && xmm8 < 4.5)
            xmm8 *= 1.06;

        float sp128 = 0.0f;
        float sp20  = 0.0f;
        float sp130 = 0.0f;
        float sp24  = 0.0f;
        float sp138 = 0.0f;
        float xmm11 = 0.0f;
        float xmm13 = 0.0f;
        float xmm14 = 0.0f;

        if (xmm9 > 1.0e-5)
        {
            // Positive Drive branch (0x500003499: Triode / Tape Asymmetric Bank 1)
            c.tableOffset = 0x4000; // 16384
            sp128 = 0.099609375f;   // [0x50003B3C8]
            sp20  = 0.359375f;      // [0x50003B3E0]
            xmm11 = 0.625f;         // [0x50003B3E8]
            xmm13 = 0.35058594f;    // [0x50003B3DC]
            xmm14 = 0.453125f;      // [0x50003B3E4]
            sp130 = 0.734375f;      // [0x50003B3EC]
            sp24  = 0.9375f;        // [0x50003B3F0]
            sp138 = 0.9921875f;     // [0x50003B3F4]
        }
        else if (xmm9 < -1.0e-5)
        {
            // Negative Drive branch (0x50000352B: Solid-State / Console Symmetric Bank 0)
            c.tableOffset = 0;
            xmm9  = -xmm9;          // abs(drive) in (0, 1]
            sp128 = 0.10546875f;    // [0x50003B3CC]
            xmm11 = 1.0f;           // [0x50003B3F8]
            xmm13 = 0.31152344f;    // [0x50003B3D8]
            xmm14 = 0.171875f;      // [0x50003B3D0]
            sp130 = 0.9921875f;     // [0x50003B3F4]
            sp24  = -0.25f;         // [0x50003B470]
            sp138 = 0.9375f;        // [0x50003B3F0]
            sp20  = 0.2421875f;     // [0x50003B3D4]
        }
        else
        {
            xmm9  = 0.0;
            xmm11 = 0.0f;
        }

        c.bypassFlag = (bypassed || xmm9 == 0.0) ? 1 : 0;
        if (c.bypassFlag != 0)
        {
            xmm9  = 0.0;
            xmm11 = 0.0f;
        }

        // 0x500003600..0x5000037A6: Exact polynomial & sqrt Ocelot parameter synthesis
        xmm8 = xmm8 * 0.125 + 0.25;
        const double xmm7 = 0.5;
        double xmm6 = (xmm9 * xmm9 + xmm9) * xmm7; // 0.5 * (d^2 + d)

        c.normScale = 1.0f / static_cast<float> (2 << 13);

        const double sqrtDrive = std::sqrt (xmm9);
        const double xmm2_d    = static_cast<double> (sp138) * xmm6;

        // c.driveDepth (+0x18): (sqrt(d) + (d - xmm6)*2.0) * sp128
        const double depthD = (sqrtDrive + (xmm9 - xmm6) * 2.0) * static_cast<double> (sp128);
        c.driveDepth = static_cast<float> (depthD);

        // c.postG (+0x28): sp130 * xmm6
        c.postG = static_cast<float> (static_cast<double> (sp130) * xmm6);

        // c.postB0 (+0x20): sp138 * xmm6
        c.postB0 = static_cast<float> (xmm2_d);

        // c.postA1 (+0x24): 1.0 - (((xmm6 + d)*0.6 + d) * 0.5 * sp20)
        const double a1Term = (((xmm6 + xmm9) * 0.6 + xmm9) * xmm7) * static_cast<double> (sp20);
        c.postA1 = 1.0f - static_cast<float> (a1Term);

        // c.postMix1 (+0x14): sp24
        c.postMix1 = sp24;

        // c.dryMakeUp (+0x1C): 1.0 + (d * 0.5 + 0.5) * 0.9375
        c.dryMakeUp = static_cast<float> ((xmm9 * xmm7 + xmm7) * 0.9375 + 1.0);

        if (xmm9 == 0.0)
        {
            c.tableOffset  = 0x4000;
            c.preCoef1     = 0.625f;
            c.preDiffCoef  = 0.7011719f;
            c.preScaleCoef = 1.453125f;
        }
        else
        {
            // Tone-dependent pre-emphasis filter coefficients (+0x04, +0x08, +0x0C)
            c.preCoef1 = static_cast<float> (static_cast<double> (xmm11) * xmm8);
            const double scaledSr = xmm10 * 2.0 * xmm8;
            c.preDiffCoef  = static_cast<float> (static_cast<double> (xmm13) * scaledSr) * 2.0f;
            c.preScaleCoef = static_cast<float> (static_cast<double> (xmm14) * scaledSr) + 1.0f;
        }

        return c;
    }

    /**
     * Exact decompiled per-sample Ocelot waveshaper + dual-tap interpolation + post-filter
     * from HEAT.aaxplugin @ 0x500001B70 (`AlgEntry`).
     */
    inline float processOcelotSample (int ch, float rawIn, const OcelotCoefPacket& c) noexcept
    {
        if (c.bypassFlag != 0)
            return rawIn;

        const float* tableBase = kOcelotWaveshaperLUT + c.tableOffset;

        // Pre-gain input scaling (0x500001CC9: 1.0 - c.driveDepth)
        const float inGain = 1.0f - c.driveDepth;
        const float x3 = rawIn * inGain;

        // Stage 1: Pre-emphasis differentiator + First LUT lookup (0x500001E51..0x500001EE9)
        // Note: At 0x500001E6C, `mulss xmm2, xmm11` multiplies by `[rcx+1Ch]` (`c.dryMakeUp`)
        float u1 = (x3 * c.preScaleCoef - stateZ0[ch]) * c.dryMakeUp - (-1.0f);
        float idx1 = juce::jlimit (0.0f, 16383.0f, u1 * 8191.5f);

        const int i1 = static_cast<int> (idx1);
        const float frac1 = idx1 - static_cast<float> (i1);
        const float lut1 = (1.0f - frac1) * tableBase[i1] + frac1 * tableBase[i1 + 1];

        const float w1 = lut1 * c.driveDepth;
        const float buf1F30 = w1 * c.preCoef1;

        // Stage 2: Second LUT lookup with harmonic feedback (0x500001ED5..0x500001F4C)
        float u2 = (x3 + w1) - (-1.0f);
        float idx2 = juce::jlimit (0.0f, 16383.0f, u2 * 8191.5f);

        const int i2 = static_cast<int> (idx2);
        const float frac2 = idx2 - static_cast<float> (i2);
        const float buf2F2C = (1.0f - frac2) * tableBase[i2] + frac2 * tableBase[i2 + 1];

        stateZ0[ch] = x3;

        // Stage 3: Post-emphasis 1-pole state filter (0x500001F80..0x5000020C5)
        const float fbCoeff = 1.0f - c.preDiffCoef;
        stateZ1[ch] = stateZ1[ch] * fbCoeff + (buf1F30 + buf2F2C) * c.preDiffCoef;
        const float buf0F2C = buf1F30 * c.postMix1 + stateZ1[ch] * c.postG;

        // Stage 4: Final harmonic recombination & output scaling (0x50000213E..0x500002163)
        // In HEAT.aaxplugin @ 0x500002157..0x50000215B: yOut = (x3 + (buf0F2C - x3 * postG * postB0)) * postA1
        const float postSub = c.postG * c.postB0;
        const float wetHarmonic = buf0F2C - x3 * postSub;
        const float yOut = (x3 + wetHarmonic) * c.postA1;

        if (currentOSMode == kOS_1x_NativePT)
            return yOut;

        const float unityComp = 1.0f / std::max (0.25f, inGain * c.postA1);
        return yOut * unityComp;
    }

    void processBlock (juce::AudioBuffer<float>& buffer) noexcept
    {
        juce::ScopedNoDenormals noDenormals;

        const int numChannels = buffer.getNumChannels();
        const int numSamples  = buffer.getNumSamples();
        if (numChannels <= 0 || numSamples <= 0)
            return;

        const int activeChannels = std::min (numChannels, 2);

        // Save dry buffer for 10 ms clickless bypass crossfade
        for (int ch = 0; ch < activeChannels; ++ch)
        {
            const int copyCount = std::min (numSamples, dryBuffer.getNumSamples());
            juce::FloatVectorOperations::copy (dryBuffer.getWritePointer (ch),
                                               buffer.getReadPointer (ch),
                                               copyCount);
        }

        juce::dsp::AudioBlock<float> baseBlock (buffer);
        auto stereoBlock = baseBlock.getSubsetChannelBlock (0, static_cast<size_t> (activeChannels));

        // Select 1x Native or 2x/4x/8x Oversampled block
        juce::dsp::Oversampling<float>* activeOS = nullptr;
        if (currentOSMode > kOS_1x_NativePT)
            activeOS = oversamplers[static_cast<size_t> (currentOSMode - 1)].get();

        juce::dsp::AudioBlock<float> procBlock = (activeOS != nullptr)
                                                    ? activeOS->processSamplesUp (stereoBlock)
                                                    : stereoBlock;

        const int procNumSamples = static_cast<int> (procBlock.getNumSamples());

        float* chPtr[2] = {
            procBlock.getChannelPointer (0),
            (activeChannels > 1) ? procBlock.getChannelPointer (1) : nullptr
        };

        float blockPeakActivity = 0.0f;
        float lastDriveVal = targetDrive;

        for (int i = 0; i < procNumSamples; ++i)
        {
            const float drive = driveSmoothed.getNextValue();
            const float tone  = toneSmoothed.getNextValue();
            lastDriveVal = drive;

            const OcelotCoefPacket coef = computeOcelotCoefficients (drive, tone, oversampledRate, false);

            for (int ch = 0; ch < activeChannels; ++ch)
            {
                const float x = chPtr[ch][i];
                float y = processOcelotSample (ch, x, coef);

                const float delta = std::abs (y - x);
                if (delta > blockPeakActivity)
                    blockPeakActivity = delta;

                if (currentOSMode == kOS_1x_NativePT)
                {
                    chPtr[ch][i] = y;
                }
                else
                {
                    // 7 Hz DC Blocker
                    const float dcOut = y - dcX1[ch] + dcBlockerR * dcY1[ch];
                    dcX1[ch] = y;
                    dcY1[ch] = dcOut;

                    chPtr[ch][i] = dcOut;
                }
            }
        }

        if (activeOS != nullptr)
            activeOS->processSamplesDown (stereoBlock);

        // Apply 10 ms clickless bypass crossfade
        const int safeDrySamples = std::min (numSamples, dryBuffer.getNumSamples());
        for (int i = 0; i < safeDrySamples; ++i)
        {
            const float wetMix = bypassMixSmoothed.getNextValue();
            const float dryMix = 1.0f - wetMix;

            if (wetMix < 0.9999f)
            {
                for (int ch = 0; ch < activeChannels; ++ch)
                {
                    float* outPtr = buffer.getWritePointer (ch);
                    const float* dryPtr = dryBuffer.getReadPointer (ch);
                    outPtr[i] = outPtr[i] * wetMix + dryPtr[i] * dryMix;
                }
            }
        }

        const float activeWeight = targetBypass ? 0.0f : 1.0f;
        const float normalizedActivity = juce::jlimit (0.0f, 1.0f, blockPeakActivity * 2.8f * activeWeight);
        const float prevActivity = harmonicActivity.load (std::memory_order_relaxed);
        const float smoothedLed = (normalizedActivity > prevActivity)
                                    ? normalizedActivity
                                    : (prevActivity * 0.86f + normalizedActivity * 0.14f);
        harmonicActivity.store (smoothedLed, std::memory_order_relaxed);
        harmonicPolarity.store (lastDriveVal, std::memory_order_relaxed);
    }

    float getHarmonicActivity() const noexcept
    {
        return harmonicActivity.load (std::memory_order_relaxed);
    }

    float getHarmonicPolarity() const noexcept
    {
        return harmonicPolarity.load (std::memory_order_relaxed);
    }

private:
    void updateRatesForMode (int osMode) noexcept
    {
        static constexpr double kFactors[4] = { 1.0, 2.0, 4.0, 8.0 };
        const double factor = kFactors[juce::jlimit (0, 3, osMode)];
        oversampledRate = baseSampleRate * factor;

        const float curDrive = driveSmoothed.getCurrentValue();
        const float curTone  = toneSmoothed.getCurrentValue();
        driveSmoothed.reset (oversampledRate, 0.020);
        toneSmoothed.reset (oversampledRate, 0.020);
        driveSmoothed.setCurrentAndTargetValue (curDrive);
        toneSmoothed.setCurrentAndTargetValue (curTone);

        const double dcR = std::exp (-2.0 * juce::MathConstants<double>::pi * 7.0 / oversampledRate);
        dcBlockerR = static_cast<float> (dcR);
    }

    double baseSampleRate  = 44100.0;
    double oversampledRate = 176400.0;
    int    currentOSMode   = kOS_4x;

    std::array<std::unique_ptr<juce::dsp::Oversampling<float>>, 3> oversamplers;

    float stateZ0[2] = { 0.0f, 0.0f };
    float stateZ1[2] = { 0.0f, 0.0f };
    float dcX1[2]    = { 0.0f, 0.0f };
    float dcY1[2]    = { 0.0f, 0.0f };
    float dcBlockerR = 0.99975f;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> driveSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> toneSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> bypassMixSmoothed;

    juce::AudioBuffer<float> dryBuffer;

    float targetDrive  = 0.0f;
    float targetTone   = 0.0f;
    bool  targetBypass = false;

    std::atomic<float> harmonicActivity { 0.0f };
    std::atomic<float> harmonicPolarity { 0.0f };
};

} // namespace flheat
