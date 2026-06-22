// xexdump — diagnostic: parse a XEX via XenonUtils, print the section map,
// and write the decrypted/decompressed flat image for Ghidra/offline analysis.
//
// Output flat file layout: raw bytes of the image as loaded at image.base,
// i.e. file offset N == virtual address (image.base + N).

#include <cstdio>
#include <file.h>
#include <image.h>
#include <section.h>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("Usage: xexdump <input.xex> <output.bin>\n");
        return 1;
    }

    const auto file = LoadFile(argv[1]);
    if (file.empty())
    {
        printf("error: cannot read %s\n", argv[1]);
        return 1;
    }

    auto image = Image::ParseImage(file.data(), file.size());
    if (!image.data)
    {
        printf("error: ParseImage failed\n");
        return 1;
    }

    printf("base:        0x%zX\n", image.base);
    printf("size:        0x%X\n", image.size);
    printf("entry point: 0x%zX\n", image.entry_point);
    printf("sections:\n");
    for (const auto& s : image.sections)
    {
        printf("  %-10s base=0x%08zX size=0x%08X flags=0x%02X%s\n",
               s.name.c_str(), s.base, s.size, s.flags,
               (s.flags & SectionFlags_Code) ? " [code]" : "");
    }

    FILE* f = fopen(argv[2], "wb");
    if (!f)
    {
        printf("error: cannot open %s\n", argv[2]);
        return 1;
    }
    fwrite(image.data.get(), 1, image.size, f);
    fclose(f);
    printf("wrote %u bytes to %s\n", image.size, argv[2]);
    return 0;
}
