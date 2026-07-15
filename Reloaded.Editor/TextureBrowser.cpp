#include "pch.h"
#include "TextureBrowser.h"
#include "Hooks.h"
#include "MemoryWriter.h"
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#include <cstdio>
#include <cstring>
#include <string>

INIT_HOOKS;

// SCCT's UnrealEd lacks DDS export support present in stock UE2.
// Restore DDS export for DXT1/DXT3/DXT5 textures in the Texture Browser.

#define HOOK_EXPORT_DISPATCH   0x10EA2035u
#define RESUME_NATIVE_OFN      0x10EA203Bu
#define SKIP_TO_CASE_EPILOGUE  0x10EA2101u

// UTexture field offsets
// SCCT Versus's UTexture layout differs from stock UE2; USize/VSize are not
// stored at the usual offsets. Compute dimensions from UBits/VBits instead.
#define UTEX_FORMAT_OFFSET     0x5C
#define UTEX_UBITS_OFFSET      0x5F
#define UTEX_VBITS_OFFSET      0x60
#define UTEX_MIPS_DATA         0x70   // TArray<FMipmap>.Data (FMipmap*)
#define UTEX_MIPS_NUM          0x74   // TArray<FMipmap>.Num
#define UTEX_NAME_OFFSET       0x20   // UObject::Name (FName index, lower 32 bits)

#define MIP_STRIDE             0x28
#define MIP_LAZYLOADER_OFFSET  0x10
#define MIP_DATAARRAY_DATA     0x1C
#define MIP_DATAARRAY_NUM      0x20

// TLazyArray<BYTE>::Load(this)
// Loads mip data into DataArray if not already resident.
typedef void (__fastcall *FArrayLoadFn)(void* lazyLoaderSubobj);
#define TLAZYARRAY_LOAD_ADDR   0x10EAB600u

#define TEXF_DXT1              3
#define TEXF_DXT3              7
#define TEXF_DXT5              8

#define GNAMES_DATA_PTR        0x1169cfbcu
#define GNAMES_NUM_PTR         0x1169cfc0u
#define FNAME_ENTRY_STR_OFFSET 0x0C

// DDS file format constants
#define DDS_MAGIC              0x20534444u   // "DDS "
#define DDSD_CAPS              0x00000001u
#define DDSD_HEIGHT            0x00000002u
#define DDSD_WIDTH             0x00000004u
#define DDSD_PIXELFORMAT       0x00001000u
#define DDSD_MIPMAPCOUNT       0x00020000u
#define DDSD_LINEARSIZE        0x00080000u
#define DDPF_FOURCC            0x00000004u
#define DDSCAPS_COMPLEX        0x00000008u
#define DDSCAPS_TEXTURE        0x00001000u
#define DDSCAPS_MIPMAP         0x00400000u

#pragma pack(push, 4)
struct DDPIXELFORMAT_t {
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwRGBAlphaBitMask;
};
struct DDSCAPS2_t {
    DWORD dwCaps;
    DWORD dwCaps2;
    DWORD dwCaps3;
    DWORD dwCaps4;
};
struct DDSURFACEDESC2_t {
    DWORD            dwSize;
    DWORD            dwFlags;
    DWORD            dwHeight;
    DWORD            dwWidth;
    DWORD            dwPitchOrLinearSize;
    DWORD            dwDepth;
    DWORD            dwMipMapCount;
    DWORD            dwReserved1[11];
    DDPIXELFORMAT_t  ddpfPixelFormat;
    DDSCAPS2_t       ddsCaps;
    DWORD            dwReserved2;
};
struct FDDSFileHeader_t {
    DWORD            Magic;
    DDSURFACEDESC2_t desc;
};
#pragma pack(pop)

static_assert(sizeof(DDPIXELFORMAT_t)  == 32,  "DDPIXELFORMAT must be 32 bytes");
static_assert(sizeof(DDSURFACEDESC2_t) == 124, "DDSURFACEDESC2 must be 124 bytes");
static_assert(sizeof(FDDSFileHeader_t) == 128, "DDS file header must be 128 bytes");

// DDSD_LINEARSIZE for DXT textures. Surfaces below 4x4 still occupy one block.
static DWORD DXT_LinearSize(BYTE format, DWORD width, DWORD height)
{
    DWORD w = (width  < 4) ? 4 : width;
    DWORD h = (height < 4) ? 4 : height;
    DWORD blockBytes = (format == TEXF_DXT1) ? 8 : 16;
    return (w / 4) * (h / 4) * blockBytes;
}

static void GetTextureObjectName(void* pTexture, char* outBuf, size_t bufSize)
{
    if (!pTexture || !outBuf || bufSize == 0) return;
    outBuf[0] = '\0';

    void* namesArray = *reinterpret_cast<void**>(GNAMES_DATA_PTR);
    INT   namesCount = *reinterpret_cast<INT*>  (GNAMES_NUM_PTR);
    if (!namesArray || namesCount <= 0) return;

    INT nameIndex = *reinterpret_cast<INT*>(static_cast<char*>(pTexture) + UTEX_NAME_OFFSET);
    if (nameIndex < 0 || nameIndex >= namesCount) return;

    void* fnameEntry = reinterpret_cast<void**>(namesArray)[nameIndex];
    if (!fnameEntry) return;

    const char* nameStr = reinterpret_cast<const char*>(
        static_cast<char*>(fnameEntry) + FNAME_ENTRY_STR_OFFSET);
    strncpy_s(outBuf, bufSize, nameStr, _TRUNCATE);
}

static BOOL WriteDDSFromUTexture(void* pTexture, const char* path)
{
    if (!pTexture || !path) return FALSE;

    BYTE  format   = *reinterpret_cast<BYTE*> (static_cast<char*>(pTexture) + UTEX_FORMAT_OFFSET);
    BYTE  ubits    = *reinterpret_cast<BYTE*> (static_cast<char*>(pTexture) + UTEX_UBITS_OFFSET);
    BYTE  vbits    = *reinterpret_cast<BYTE*> (static_cast<char*>(pTexture) + UTEX_VBITS_OFFSET);
    void* mipsData = *reinterpret_cast<void**>(static_cast<char*>(pTexture) + UTEX_MIPS_DATA);
    INT   mipsNum  = *reinterpret_cast<INT*>  (static_cast<char*>(pTexture) + UTEX_MIPS_NUM);

    if (format != TEXF_DXT1 && format != TEXF_DXT3 && format != TEXF_DXT5) return FALSE;
    if (ubits > 13 || vbits > 13)                                          return FALSE;
    if (!mipsData || mipsNum <= 0)                                         return FALSE;

    INT usize = 1 << ubits;
    INT vsize = 1 << vbits;

    FDDSFileHeader_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.Magic                            = DDS_MAGIC;
    hdr.desc.dwSize                      = sizeof(DDSURFACEDESC2_t);
    hdr.desc.dwFlags                     = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH
                                         | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE;
    hdr.desc.dwWidth                     = static_cast<DWORD>(usize);
    hdr.desc.dwHeight                    = static_cast<DWORD>(vsize);
    hdr.desc.dwMipMapCount               = static_cast<DWORD>(mipsNum);
    hdr.desc.dwPitchOrLinearSize         = DXT_LinearSize(format,
                                              static_cast<DWORD>(usize),
                                              static_cast<DWORD>(vsize));
    hdr.desc.ddpfPixelFormat.dwSize      = sizeof(DDPIXELFORMAT_t);
    hdr.desc.ddpfPixelFormat.dwFlags     = DDPF_FOURCC;
    hdr.desc.ddpfPixelFormat.dwFourCC    =
        (format == TEXF_DXT1) ? ('D' | ('X' << 8) | ('T' << 16) | ('1' << 24)) :
        (format == TEXF_DXT3) ? ('D' | ('X' << 8) | ('T' << 16) | ('3' << 24)) :
                                ('D' | ('X' << 8) | ('T' << 16) | ('5' << 24));
    hdr.desc.ddsCaps.dwCaps              = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;

    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return FALSE;

    DWORD written = 0;
    if (!WriteFile(hFile, &hdr, sizeof(hdr), &written, nullptr) || written != sizeof(hdr))
    {
        CloseHandle(hFile);
        DeleteFileA(path);
        return FALSE;
    }

    // Ensure mip data is resident before writing.
    FArrayLoadFn TLazyArray_Load = reinterpret_cast<FArrayLoadFn>(
        static_cast<uintptr_t>(TLAZYARRAY_LOAD_ADDR));

    char* mipCursor = static_cast<char*>(mipsData);
    for (INT m = 0; m < mipsNum; ++m)
    {
        TLazyArray_Load(mipCursor + MIP_LAZYLOADER_OFFSET);

        void* dataPtr = *reinterpret_cast<void**>(mipCursor + MIP_DATAARRAY_DATA);
        INT   dataNum = *reinterpret_cast<INT*>  (mipCursor + MIP_DATAARRAY_NUM);
        if (!dataPtr || dataNum <= 0)
        {
            CloseHandle(hFile);
            DeleteFileA(path);
            return FALSE;
        }
        if (!WriteFile(hFile, dataPtr, static_cast<DWORD>(dataNum), &written, nullptr) ||
            written != static_cast<DWORD>(dataNum))
        {
            CloseHandle(hFile);
            DeleteFileA(path);
            return FALSE;
        }
        mipCursor += MIP_STRIDE;
    }

    CloseHandle(hFile);
    return TRUE;
}

static int __cdecl TB_RunDDSExport(void* pTexture, HWND hParent)
{
    if (!pTexture) return 0;

    BYTE fmt = *reinterpret_cast<BYTE*>(static_cast<char*>(pTexture) + UTEX_FORMAT_OFFSET);
    if (fmt != TEXF_DXT1 && fmt != TEXF_DXT3 && fmt != TEXF_DXT5)
        return 0;

    char defaultName[256] = {};
    GetTextureObjectName(pTexture, defaultName, sizeof(defaultName));
    if (defaultName[0] == '\0')
        strncpy_s(defaultName, "texture", _TRUNCATE);

    char filePath[MAX_PATH * 2] = {};
    snprintf(filePath, sizeof(filePath), "%s.dds", defaultName);

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hParent;
    ofn.lpstrFilter = "DirectDraw Surface (*.dds)\0*.dds\0All Files (*.*)\0*.*\0";
    ofn.lpstrDefExt = "dds";
    ofn.lpstrFile   = filePath;
    ofn.nMaxFile    = sizeof(filePath);
    ofn.lpstrTitle  = "Export Texture";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;

    if (!GetSaveFileNameA(&ofn))
        return 1;

    if (!WriteDDSFromUTexture(pTexture, filePath))
    {
        char msg[MAX_PATH + 128];
        snprintf(msg, sizeof(msg),
                 "Failed to export DDS to:\n%s\n\n"
                 "Texture mip data may not be loaded.", filePath);
        MessageBoxA(hParent, msg, "Export Texture", MB_OK | MB_ICONERROR);
    }
    return 1;
}

JMP_HOOK(HOOK_EXPORT_DISPATCH, TB_ExportDispatchHook)
{
    static int resumeNative = static_cast<int>(RESUME_NATIVE_OFN);
    static int skipToEpilog = static_cast<int>(SKIP_TO_CASE_EPILOGUE);

    __asm {
        mov  al, byte ptr [esi + UTEX_FORMAT_OFFSET]
        cmp  al, TEXF_DXT1
        je   handle_dds
        cmp  al, TEXF_DXT3
        je   handle_dds
        cmp  al, TEXF_DXT5
        je   handle_dds

        // Resume native export path.
        lea  eax, [ebp + 0xffffff18]
        jmp  dword ptr [resumeNative]

    handle_dds:
        pushad
        pushfd

        push dword ptr [ebp + 0xffffff1c]  // HWND
        push esi                           // UTexture*
        call TB_RunDDSExport
        add  esp, 8
        popfd
        popad
        jmp  dword ptr [skipToEpilog]
    }
}

void TextureBrowser::Initialize()
{
    INSTALL_HOOKS;
}
