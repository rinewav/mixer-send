// Mixer SEND - Copyright (C) 2026 Covo Design
// SPDX-License-Identifier: AGPL-3.0-only

/*
    Renders the Mixer SEND editor off-screen to PNG at 100% and 200%, in the
    states that change what it draws (host name + colour, no name, linked /
    not linked is whatever the machine has, muted, extreme values).

    No DAW and no screen needed, so layout breakage shows up on every build.

    usage: mixer_send_snapshot [output directory]
*/

#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"
#include "plugin/Parameters.h"

#include <juce_gui_basics/juce_gui_basics.h>

using namespace juce;

namespace
{
    void setParameter (mixersend::MixerSendProcessor& processor, const char* id, float value)
    {
        if (auto* p = processor.state().getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
    }

    bool writeSnapshot (mixersend::MixerSendProcessor& processor, const File& outDir, const String& name, float scale)
    {
        std::unique_ptr<AudioProcessorEditor> editor (processor.createEditor());

        if (editor == nullptr)
            return false;

        editor->setSize (mixersend::MixerSendEditor::baseWidth, mixersend::MixerSendEditor::baseHeight);
        MessageManager::getInstance()->runDispatchLoopUntil (150); // let the status timer run once

        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, scale);

        if (! image.isValid())
            return false;

        const auto file = outDir.getChildFile (name + (scale > 1.5f ? "@2x" : "") + ".png");
        file.deleteFile();
        FileOutputStream stream (file);

        if (! stream.openedOk() || ! PNGImageFormat().writeImageToStream (image, stream))
            return false;

        std::printf ("  %-26s %4d x %-4d  %s\n", (name + (scale > 1.5f ? " @2x" : "")).toRawUTF8(),
                     image.getWidth(), image.getHeight(), file.getFullPathName().toRawUTF8());
        return true;
    }
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI juceInit;

    const File outDir = argc > 1 ? File (String (argv[1]))
                                 : File::getCurrentWorkingDirectory().getChildFile ("snapshots");
    outDir.createDirectory();

    std::printf ("Mixer SEND UI snapshots -> %s\n\n", outDir.getFullPathName().toRawUTF8());

    bool ok = true;

    {
        mixersend::MixerSendProcessor processor;
        processor.prepareToPlay (48000.0, 512);
        juce::AudioProcessor::TrackProperties props;
        props.name = String ("Lead Vocal");
        props.colourARGB = 0xff3a7bd5u;
        processor.updateTrackProperties (props);
        setParameter (processor, mixersend::params::volumeId, 0.76f);
        setParameter (processor, mixersend::params::panId, -30.0f);

        for (const float scale : { 1.0f, 2.0f })
            ok &= writeSnapshot (processor, outDir, "named-unity", scale);
    }

    {
        mixersend::MixerSendProcessor processor;
        processor.prepareToPlay (48000.0, 512);
        setParameter (processor, mixersend::params::volumeId, 0.3f);
        setParameter (processor, mixersend::params::panId, 100.0f);
        setParameter (processor, mixersend::params::muteId, 1.0f);

        for (const float scale : { 1.0f, 2.0f })
            ok &= writeSnapshot (processor, outDir, "unnamed-muted", scale);
    }

    {
        mixersend::MixerSendProcessor processor;
        juce::AudioProcessor::TrackProperties props;
        props.name = String (CharPointer_UTF8 ("\xe3\x83\x89\xe3\x83\xa9\xe3\x83\xa0\xe3\x82\xb9 Overhead Left Very Long Track Name"));
        props.colourARGB = 0xffe0a030u;
        processor.updateTrackProperties (props);
        setParameter (processor, mixersend::params::volumeId, 1.0f);

        ok &= writeSnapshot (processor, outDir, "long-name-top", 1.0f);
    }

    std::printf ("\n%s\n", ok ? "done" : "FAILED");
    return ok ? 0 : 1;
}
