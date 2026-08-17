#include "DemoEdit.h"

namespace
{

void printUsage()
{
    std::cout << "orionish-te-render — tracktion_engine spike renderer\n"
                 "\n"
                 "Usage:\n"
                 "  orionish-te-render --demo <out.wav>              render the built-in demo song\n"
                 "  orionish-te-render --write-demo <out.tracktionedit>  save the demo song as an edit file\n"
                 "  orionish-te-render <in.tracktionedit> <out.wav>  render an edit file\n";
}

juce::File resolveFile (const juce::String& path)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile (path);
}

int renderEditToWav (te::Edit& edit, const juce::File& outputFile)
{
    // Render the whole edit plus a second of tail, all tracks, no UI thread.
    const auto endTime = te::TimePosition::fromSeconds (edit.getLength().inSeconds() + 1.0);

    juce::BigInteger tracksToDo;
    for (int i = 0; i < te::getAllTracks (edit).size(); ++i)
        tracksToDo.setBit (i);

    outputFile.deleteFile();
    if (! te::Renderer::renderToFile ("Render", outputFile, edit,
                                      { te::TimePosition(), endTime },
                                      tracksToDo, true, true, {}, false))
    {
        std::cerr << "Render failed\n";
        return 1;
    }

    std::cout << "Rendered \"" << edit.getName() << "\" -> "
              << outputFile.getFullPathName() << "\n";
    return 0;
}

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    juce::StringArray args (argv + 1, argc - 1);

    if (args.size() != 2)
    {
        printUsage();
        return args.isEmpty() ? 0 : 1;
    }

    te::Engine engine { "orionish-te" };

    if (args[0] == "--demo")
    {
        auto edit = orionish::buildDemoEdit (engine);
        return renderEditToWav (*edit, resolveFile (args[1]));
    }

    if (args[0] == "--write-demo")
    {
        auto file = resolveFile (args[1]);
        auto edit = orionish::buildDemoEdit (engine);
        if (! te::EditFileOperations (*edit).saveAs (file, true))
        {
            std::cerr << "Failed to write " << file.getFullPathName() << "\n";
            return 1;
        }
        std::cout << "Wrote edit -> " << file.getFullPathName() << "\n";
        return 0;
    }

    auto editFile = resolveFile (args[0]);
    if (! editFile.existsAsFile())
    {
        std::cerr << "Could not load edit file: " << args[0] << "\n";
        return 1;
    }

    auto edit = te::loadEditFromFile (engine, editFile);
    if (edit == nullptr)
    {
        std::cerr << "Could not parse edit file: " << args[0] << "\n";
        return 1;
    }
    return renderEditToWav (*edit, resolveFile (args[1]));
}
