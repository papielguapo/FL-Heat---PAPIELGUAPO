#include "PluginProcessor.h"
#include "PluginEditor.h"

FLHeatAudioProcessor::FLHeatAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "FLHeatState", createParameterLayout())
{
    driveParam  = apvts.getRawParameterValue (kParamDrive);
    toneParam   = apvts.getRawParameterValue (kParamTone);
    bypassParam = apvts.getRawParameterValue (kParamBypass);
    linkParam   = apvts.getRawParameterValue (kParamLink);
    skinParam   = apvts.getRawParameterValue (kParamSkin);
    osParam     = apvts.getRawParameterValue (kParamOS);

    apvts.addParameterListener (kParamDrive,  this);
    apvts.addParameterListener (kParamTone,   this);
    apvts.addParameterListener (kParamBypass, this);
    apvts.addParameterListener (kParamLink,   this);
    apvts.addParameterListener (kParamOS,     this);
}

FLHeatAudioProcessor::~FLHeatAudioProcessor()
{
    apvts.removeParameterListener (kParamDrive,  this);
    apvts.removeParameterListener (kParamTone,   this);
    apvts.removeParameterListener (kParamBypass, this);
    apvts.removeParameterListener (kParamLink,   this);
    apvts.removeParameterListener (kParamOS,     this);
}

juce::AudioProcessorValueTreeState::ParameterLayout FLHeatAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { kParamDrive, 1 },
        "Drive",
        juce::NormalisableRange<float> (-10.0f, 10.0f, 0.01f),
        0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int)
            {
                if (std::abs (v) < 0.005f)
                    return juce::String ("0.0");
                return (v > 0.0f ? "+" : "") + juce::String (v, 1);
            })
            .withValueFromStringFunction ([] (const juce::String& text)
            {
                return text.trimCharactersAtStart ("+").getFloatValue();
            })));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { kParamTone, 1 },
        "Tone",
        juce::NormalisableRange<float> (-10.0f, 10.0f, 0.01f),
        0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int)
            {
                if (std::abs (v) < 0.005f)
                    return juce::String ("0.0");
                return (v > 0.0f ? "+" : "") + juce::String (v, 1);
            })
            .withValueFromStringFunction ([] (const juce::String& text)
            {
                return text.trimCharactersAtStart ("+").getFloatValue();
            })));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { kParamBypass, 1 },
        "Bypass",
        false));

    // LNK ON (true) by default so dropping multiple instances across FL Studio / Ableton tracks
    // links them automatically like Pro Tools' global HEAT mixer console!
    params.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { kParamLink, 1 },
        "Link Mode (IND / LNK)",
        true));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { kParamSkin, 1 },
        "Skin Style",
        juce::StringArray {
            "FL Studio Dark Mixer",
            "Pro Tools HEAT 1:1",
            "Atari ST / Retro PC",
            "Ableton Live 12",
            "Apple Logic Pro X",
            "Steinberg Cubase 14"
        },
        0));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { kParamOS, 1 },
        "Oversampling",
        juce::StringArray { "1x (Pro Tools Native 0-Latency)", "2x Oversampling", "4x Oversampling", "8x Oversampling" },
        2));

    return { params.begin(), params.end() };
}

const juce::String FLHeatAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool FLHeatAudioProcessor::acceptsMidi() const  { return false; }
bool FLHeatAudioProcessor::producesMidi() const { return false; }
bool FLHeatAudioProcessor::isMidiEffect() const { return false; }
double FLHeatAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int FLHeatAudioProcessor::getNumPrograms() { return 1; }
int FLHeatAudioProcessor::getCurrentProgram() { return 0; }
void FLHeatAudioProcessor::setCurrentProgram (int index) { juce::ignoreUnused (index); }
const juce::String FLHeatAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return "Default";
}
void FLHeatAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

void FLHeatAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    heatDSP.prepare (sampleRate, samplesPerBlock);
    setLatencySamples (heatDSP.getLatencySamples (getOversamplingMode()));
}

void FLHeatAudioProcessor::releaseResources()
{
    heatDSP.reset();
}

bool FLHeatAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainIn  = layouts.getMainInputChannelSet();
    const auto& mainOut = layouts.getMainOutputChannelSet();

    if (mainIn != mainOut)
        return false;

    return (mainOut == juce::AudioChannelSet::mono()
         || mainOut == juce::AudioChannelSet::stereo());
}

bool FLHeatAudioProcessor::isLinkedMode() const noexcept
{
    return linkParam != nullptr && linkParam->load (std::memory_order_relaxed) >= 0.5f;
}

int FLHeatAudioProcessor::getSkinIndex() const noexcept
{
    if (skinParam == nullptr)
        return 0;
    return juce::jlimit (0, static_cast<int> (flheat::kNumSkins) - 1,
                         static_cast<int> (std::lround (skinParam->load (std::memory_order_relaxed))));
}

void FLHeatAudioProcessor::setSkinIndex (int newSkinIdx) noexcept
{
    if (auto* p = apvts.getParameter (kParamSkin))
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (juce::jlimit (0, static_cast<int> (flheat::kNumSkins) - 1, newSkinIdx))));
}

void FLHeatAudioProcessor::cycleSkin() noexcept
{
    setSkinIndex ((getSkinIndex() + 1) % static_cast<int> (flheat::kNumSkins));
}

int FLHeatAudioProcessor::getOversamplingMode() const noexcept
{
    if (osParam == nullptr)
        return 2;
    return juce::jlimit (0, 3, static_cast<int> (std::lround (osParam->load (std::memory_order_relaxed))));
}

void FLHeatAudioProcessor::setOversamplingMode (int newOSIdx) noexcept
{
    const int clamped = juce::jlimit (0, 3, newOSIdx);
    if (auto* p = apvts.getParameter (kParamOS))
        p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (clamped)));
    setLatencySamples (heatDSP.getLatencySamples (clamped));
}

void FLHeatAudioProcessor::parameterChanged (const juce::String& parameterID, float newValue)
{
    if (parameterID == kParamOS)
    {
        const int osMode = juce::jlimit (0, 3, static_cast<int> (std::lround (newValue)));
        setLatencySamples (heatDSP.getLatencySamples (osMode));
    }

    if (isSyncingFromSharedBus.load (std::memory_order_acquire))
        return;

    if (parameterID == kParamLink)
    {
        if (newValue >= 0.5f)
        {
            const uint64_t busRev = sharedBus->revision.load (std::memory_order_acquire);
            if (busRev <= 1)
                pushLocalToSharedBus();
            else
                pullSharedBusIfNeeded();
        }
        return;
    }

    if (! isLinkedMode())
        return;

    if (parameterID == kParamDrive)
    {
        const float curShared = sharedBus->sharedDrive.load (std::memory_order_relaxed);
        if (std::abs (curShared - newValue) > 1.0e-5f)
        {
            sharedBus->sharedDrive.store (newValue, std::memory_order_relaxed);
            sharedBus->revision.fetch_add (1, std::memory_order_release);
            lastObservedRevision = sharedBus->revision.load (std::memory_order_acquire);
        }
    }
    else if (parameterID == kParamTone)
    {
        const float curShared = sharedBus->sharedTone.load (std::memory_order_relaxed);
        if (std::abs (curShared - newValue) > 1.0e-5f)
        {
            sharedBus->sharedTone.store (newValue, std::memory_order_relaxed);
            sharedBus->revision.fetch_add (1, std::memory_order_release);
            lastObservedRevision = sharedBus->revision.load (std::memory_order_acquire);
        }
    }
    else if (parameterID == kParamBypass)
    {
        const bool newByp = (newValue >= 0.5f);
        const bool curShared = sharedBus->sharedBypass.load (std::memory_order_relaxed);
        if (curShared != newByp)
        {
            sharedBus->sharedBypass.store (newByp, std::memory_order_relaxed);
            sharedBus->revision.fetch_add (1, std::memory_order_release);
            lastObservedRevision = sharedBus->revision.load (std::memory_order_acquire);
        }
    }
    else if (parameterID == kParamOS)
    {
        const int newOS = juce::jlimit (0, 3, static_cast<int> (std::lround (newValue)));
        const int curShared = sharedBus->sharedOS.load (std::memory_order_relaxed);
        if (curShared != newOS)
        {
            sharedBus->sharedOS.store (newOS, std::memory_order_relaxed);
            sharedBus->revision.fetch_add (1, std::memory_order_release);
            lastObservedRevision = sharedBus->revision.load (std::memory_order_acquire);
        }
    }
}

void FLHeatAudioProcessor::pushLocalToSharedBus() noexcept
{
    if (driveParam == nullptr || toneParam == nullptr || bypassParam == nullptr || osParam == nullptr)
        return;

    sharedBus->sharedDrive.store (driveParam->load (std::memory_order_relaxed), std::memory_order_relaxed);
    sharedBus->sharedTone.store (toneParam->load (std::memory_order_relaxed), std::memory_order_relaxed);
    sharedBus->sharedBypass.store (bypassParam->load (std::memory_order_relaxed) >= 0.5f, std::memory_order_relaxed);
    sharedBus->sharedOS.store (getOversamplingMode(), std::memory_order_relaxed);
    sharedBus->revision.fetch_add (1, std::memory_order_release);
    lastObservedRevision = sharedBus->revision.load (std::memory_order_acquire);
}

void FLHeatAudioProcessor::pullSharedBusIfNeeded() noexcept
{
    if (! isLinkedMode())
        return;

    const float targetDrive  = sharedBus->sharedDrive.load (std::memory_order_relaxed);
    const float targetTone   = sharedBus->sharedTone.load (std::memory_order_relaxed);
    const bool  targetBypass = sharedBus->sharedBypass.load (std::memory_order_relaxed);
    const int   targetOS     = sharedBus->sharedOS.load (std::memory_order_relaxed);

    const float curDrive  = driveParam->load (std::memory_order_relaxed);
    const float curTone   = toneParam->load (std::memory_order_relaxed);
    const bool  curBypass = (bypassParam->load (std::memory_order_relaxed) >= 0.5f);
    const int   curOS     = getOversamplingMode();

    const bool driveDiff  = std::abs (curDrive - targetDrive) > 1.0e-5f;
    const bool toneDiff   = std::abs (curTone - targetTone) > 1.0e-5f;
    const bool bypassDiff = (curBypass != targetBypass);
    const bool osDiff     = (curOS != targetOS);

    if (! driveDiff && ! toneDiff && ! bypassDiff && ! osDiff)
        return;

    isSyncingFromSharedBus.store (true, std::memory_order_release);

    if (driveDiff)
    {
        if (auto* p = apvts.getParameter (kParamDrive))
            p->setValueNotifyingHost (p->convertTo0to1 (targetDrive));
    }

    if (toneDiff)
    {
        if (auto* p = apvts.getParameter (kParamTone))
            p->setValueNotifyingHost (p->convertTo0to1 (targetTone));
    }

    if (bypassDiff)
    {
        if (auto* p = apvts.getParameter (kParamBypass))
            p->setValueNotifyingHost (targetBypass ? 1.0f : 0.0f);
    }

    if (osDiff)
    {
        if (auto* p = apvts.getParameter (kParamOS))
            p->setValueNotifyingHost (p->convertTo0to1 (static_cast<float> (targetOS)));
        setLatencySamples (heatDSP.getLatencySamples (targetOS));
    }

    lastObservedRevision = sharedBus->revision.load (std::memory_order_acquire);
    isSyncingFromSharedBus.store (false, std::memory_order_release);
}

void FLHeatAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const int totalNumInputChannels  = getTotalNumInputChannels();
    const int totalNumOutputChannels = getTotalNumOutputChannels();

    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    const bool linked = isLinkedMode();

    const float activeDrive  = linked ? sharedBus->sharedDrive.load (std::memory_order_relaxed)
                                      : driveParam->load (std::memory_order_relaxed);
    const float activeTone   = linked ? sharedBus->sharedTone.load (std::memory_order_relaxed)
                                      : toneParam->load (std::memory_order_relaxed);
    const bool  activeBypass = linked ? sharedBus->sharedBypass.load (std::memory_order_relaxed)
                                      : (bypassParam->load (std::memory_order_relaxed) >= 0.5f);
    const int   activeOS     = linked ? sharedBus->sharedOS.load (std::memory_order_relaxed)
                                      : getOversamplingMode();

    heatDSP.setParameters (activeDrive, activeTone, activeBypass, activeOS);
    heatDSP.processBlock (buffer);
}

void FLHeatAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);
    juce::ScopedNoDenormals noDenormals;

    const bool linked = isLinkedMode();
    const float activeDrive = linked ? sharedBus->sharedDrive.load (std::memory_order_relaxed)
                                     : driveParam->load (std::memory_order_relaxed);
    const float activeTone  = linked ? sharedBus->sharedTone.load (std::memory_order_relaxed)
                                     : toneParam->load (std::memory_order_relaxed);
    const int   activeOS    = linked ? sharedBus->sharedOS.load (std::memory_order_relaxed)
                                     : getOversamplingMode();

    heatDSP.setParameters (activeDrive, activeTone, true, activeOS);
    heatDSP.processBlock (buffer);
}

juce::AudioProcessorParameter* FLHeatAudioProcessor::getBypassParameter() const
{
    return apvts.getParameter (kParamBypass);
}

bool FLHeatAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* FLHeatAudioProcessor::createEditor()
{
    return new FLHeatAudioProcessorEditor (*this);
}

void FLHeatAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("editorW", savedEditorWidth.load (std::memory_order_relaxed), nullptr);
    state.setProperty ("editorH", savedEditorHeight.load (std::memory_order_relaxed), nullptr);
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    if (xml != nullptr)
        copyXmlToBinary (*xml, destData);
}

void FLHeatAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName (apvts.state.getType()))
    {
        auto tree = juce::ValueTree::fromXml (*xmlState);
        if (tree.hasProperty ("editorW"))
            savedEditorWidth.store (static_cast<int> (tree.getProperty ("editorW")), std::memory_order_relaxed);
        if (tree.hasProperty ("editorH"))
            savedEditorHeight.store (static_cast<int> (tree.getProperty ("editorH")), std::memory_order_relaxed);

        apvts.replaceState (tree);
        setLatencySamples (heatDSP.getLatencySamples (getOversamplingMode()));
        if (isLinkedMode())
            pushLocalToSharedBus();
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FLHeatAudioProcessor();
}
