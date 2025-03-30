
static void LoadEmojiFont(const std::string &emojiFontPath, ImVector<unsigned char> &emojiBuffer)
{
    if (std::filesystem::exists(emojiFontPath))
    {
        std::ifstream emojiFile(emojiFontPath.c_str(), std::ios::binary | std::ios::ate);
        std::streamsize size = emojiFile.tellg();
        emojiFile.seekg(0, std::ios::beg);

        emojiBuffer.resize((int)size);
        if (!emojiFile.read((char *)emojiBuffer.Data, size))
        {
            emojiBuffer.clear();
        }
    }
}

void SetDefaultFont(TerminalOptions &options, const std::filesystem::path &basePath)
{
    using std::filesystem::exists;

    auto fontDefault = basePath / "JetBrains Mono Regular Nerd Font Complete Mono.ttf";
    auto fontBold = basePath / "JetBrains Mono Italic Nerd Font Complete Mono.ttf";
    auto fontItalic = basePath / "JetBrains Mono Bold Nerd Font Complete Mono.ttf";
    auto fontBoldItalic = basePath / "JetBrains Mono Bold Italic Nerd Font Complete Mono.ttf";
    auto emojiFontPath = basePath / "NotoColorEmoji.ttf";

    if (exists(fontDefault))
    {
        options.fontSize = 20;
        options.font = fontDefault.string();
        if (exists(fontBold))
            options.fontBold = fontBold.string();
        if (exists(fontItalic))
            options.fontItalic = fontItalic.string();
        if (exists(fontBoldItalic))
            options.fontBoldItalic = fontBoldItalic.string();
        if (exists(emojiFontPath))
            options.fontEmoji = emojiFontPath.string();
    }
}
