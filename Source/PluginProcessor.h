#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "HeatDSP.h"
#include <atomic>

namespace flheat
{

/**
 * Thread-safe Shared Resource across all plugin instances inside FL Studio / Ableton Live.
 * Replicates Pro Tools' global HEAT console link when an instance is set to LNK mode.
 */
struct SharedHeatBus
{
    std::atomic<float> sharedDrive   { 0.0f };
    std::atomic<float> sharedTone    { 0.0f };
    std::atomic<bool>  sharedBypass  { false };
    std::atomic<int>   sharedOS      { 2 }; // Default 4x
    std::atomic<uint64_t> revision   { 1 };
};

} // namespace flheat

class FLHeatAudioProcessor : public juce::AudioProcessor,
                             private juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr const char* kParamDrive  = "drive";
    static constexpr const char* kParamTone   = "tone";
    static constexpr const char* kParamBypass = "bypass";
    static constexpr const char* kParamLink   = "link";
    static constexpr const char* kParamSkin   = "skin";
    static constexpr const char* kParamOS     = "oversampling";

    FLHeatAudioProcessor();
    ~FLHeatAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override;

    juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return apvts; }

    float getHarmonicActivity() const noexcept { return heatDSP.getHarmonicActivity(); }
    float getHarmonicPolarity() const noexcept { return heatDSP.getHarmonicPolarity(); }

    bool isLinkedMode() const noexcept;

    int getSkinIndex() const noexcept;
    void setSkinIndex (int newSkinIdx) noexcept;
    void cycleSkin() noexcept;

    int getOversamplingMode() const noexcept;
    void setOversamplingMode (int newOSIdx) noexcept;

    int getSavedEditorWidth() const noexcept  { return savedEditorWidth.load (std::memory_order_relaxed); }
    int getSavedEditorHeight() const noexcept { return savedEditorHeight.load (std::memory_order_relaxed); }
    void setSavedEditorSize (int w, int h) noexcept
    {
        savedEditorWidth.store (w, std::memory_order_relaxed);
        savedEditorHeight.store (h, std::memory_order_relaxed);
    }

    /**
     * Pulls any external shared bus updates into this instance's APVTS parameters
     * when in LNK mode. Safe to call from UI timer (MessageManager thread) and
     * guarded against infinite feedback loops via atomic flag + 1e-5f epsilon check.
     */
    void pullSharedBusIfNeeded() noexcept;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void pushLocalToSharedBus() noexcept;

    juce::AudioProcessorValueTreeState apvts;
    juce::SharedResourcePointer<flheat::SharedHeatBus> sharedBus;
    flheat::HeatDSP heatDSP;

    std::atomic<float>* driveParam  = nullptr;
    std::atomic<float>* toneParam   = nullptr;
    std::atomic<float>* bypassParam = nullptr;
    std::atomic<float>* linkParam   = nullptr;
    std::atomic<float>* skinParam   = nullptr;
    std::atomic<float>* osParam     = nullptr;

    std::atomic<bool> isSyncingFromSharedBus { false };
    uint64_t lastObservedRevision = 0;

    std::atomic<int> savedEditorWidth  { 168 };
    std::atomic<int> savedEditorHeight { 310 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FLHeatAudioProcessor)
};
