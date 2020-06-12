
/* Copyright 2020 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "utils/BaseUtil.h"
#include "utils/FileUtil.h"
#include "utils/GdiplusUtil.h"
#include "utils/ByteReader.h"
#include "utils/Archive.h"
#include "utils/PalmDbReader.h"
#include "utils/FileTypeSniff.h"

// TODO: move those functions here
extern bool IsPdfFileName(const WCHAR* path);
extern bool IsPdfFileContent(std::span<u8> d);
extern bool IsEngineMultiFileName(const WCHAR* path);
extern bool IsXpsArchive(const WCHAR* path);
extern bool IsDjVuFileName(const WCHAR* path);
extern bool IsImageEngineSupportedFile(const WCHAR* fileName, bool sniff);

// TODO: replace with an enum class FileKind { Unknown, PDF, ... };
Kind kindFilePDF = "filePDF";
Kind kindFilePS = "filePS";
Kind kindFileVbkm = "fileVbkm";
Kind kindFileXps = "fileXPS";
Kind kindFileDjVu = "fileDjVu";
Kind kindFileChm = "fileChm";
Kind kindFilePng = "filePng";
Kind kindFileJpeg = "fileJpeg";
Kind kindFileGif = "fileGif";
Kind kindFileTiff = "fileTiff";
Kind kindFileBmp = "fileBmp";
Kind kindFileTga = "fileTga";
Kind kindFileJxr = "fileJxr";
Kind kindFileHdp = "fileHdp";
Kind kindFileWdp = "fileWdp";
Kind kindFileWebp = "fileWebp";
Kind kindFileJp2 = "fileJp2";
Kind kindFileCbz = "fileCbz";
Kind kindFileCbr = "fileCbr";
Kind kindFileCb7 = "fileCb7";
Kind kindFileCbt = "fileCbt";
Kind kindFileZip = "fileZip";
Kind kindFileRar = "fileRar";
Kind kindFile7Z = "file7Z";
Kind kindFileTar = "fileTar";
Kind kindFileFb2 = "fileFb2";
Kind kindFileDir = "fileDir";
Kind kindFileEpub = "fileEpub";
Kind kindFileMobi = "fileMobi";

// .fb2.zip etc. must be first so that it isn't classified as .zip
static const char* gFileExts =
    ".fb2.zip\0"
    ".ps.gz\0"
    ".ps\0"
    ".eps\0"
    ".vbkm\0"
    ".fb2\0"
    ".fb2z\0"
    ".zfb2\0"
    ".cbz\0"
    ".cbr\0"
    ".cb7\0"
    ".cbt\0"
    ".zip\0"
    ".rar\0"
    ".7z\0"
    ".tar\0"
    ".pdf\0"
    ".xps\0"
    ".oxps\0"
    ".chm\0"
    ".png\0"
    ".jpg\0"
    ".jpeg\0"
    ".gif\0"
    ".tif\0"
    ".tiff\0"
    ".bmp\0"
    ".tga\0"
    ".jxr\0"
    ".hdp\0"
    ".wdp\0"
    ".webp\0"
    ".epub\0"
    ".mobi\0"
    ".prc\0"
    ".azw\0"
    ".azw1\0"
    ".azw3\0"
    ".jp2\0"
    "\0";

static Kind gExtsKind[] = {
    kindFileFb2,  kindFilePS,   kindFilePS,   kindFilePS,   kindFileVbkm, kindFileFb2,  kindFileFb2,  kindFileFb2,
    kindFileCbz,  kindFileCbr,  kindFileCb7,  kindFileCbt,  kindFileZip,  kindFileRar,  kindFile7Z,   kindFileTar,
    kindFilePDF,  kindFileXps,  kindFileXps,  kindFileChm,  kindFilePng,  kindFileJpeg, kindFileJpeg, kindFileGif,
    kindFileTiff, kindFileTiff, kindFileBmp,  kindFileTga,  kindFileJxr,  kindFileHdp,  kindFileWdp,  kindFileWebp,
    kindFileEpub, kindFileMobi, kindFileMobi, kindFileMobi, kindFileMobi, kindFileMobi, kindFileJp2,
};

static Kind GetKindByFileExt(const WCHAR* path) {
    AutoFree pathA = strconv::WstrToUtf8(path);
    int idx = 0;
    const char* curr = gFileExts;
    while (curr && *curr) {
        if (str::EndsWithI(pathA.Get(), curr)) {
            int n = (int)dimof(gExtsKind);
            CrashIf(idx >= n);
            if (idx >= n) {
                return nullptr;
            }
            return gExtsKind[idx];
        }
        curr = seqstrings::SkipStr(curr);
        idx++;
    }
    return nullptr;
}

// ensure gFileExts and gExtsKind match
static bool gDidVerifyExtsMatch = false;
static void VerifyExtsMatch() {
    if (gDidVerifyExtsMatch) {
        return;
    }
    CrashAlwaysIf(kindFileJp2 != GetKindByFileExt(L"foo.JP2"));
    gDidVerifyExtsMatch = true;
}

static Kind imageEngineKinds[] = {
    kindFilePng, kindFileJpeg, kindFileGif, kindFileTiff, kindFileBmp, kindFileTga,
    kindFileJxr, kindFileHdp,  kindFileWdp, kindFileWebp, kindFileJp2,
};

static bool KindInArray(Kind* kinds, int nKinds, Kind kind) {
    for (int i = 0; i < nKinds; i++) {
        Kind k = kinds[i];
        if (k == kind) {
            return true;
        }
    }
    return false;
}

bool IsImageEngineKind(Kind kind) {
    int n = dimof(imageEngineKinds);
    return KindInArray(imageEngineKinds, n, kind);
}

static Kind cbxKinds[] = {
    kindFileCbz, kindFileCbr, kindFileCb7, kindFileCbt, kindFileZip, kindFileRar, kindFile7Z, kindFileTar,
};

bool IsCbxEngineKind(Kind kind) {
    int n = dimof(cbxKinds);
    return KindInArray(cbxKinds, n, kind);
}

#define FILE_SIGS(V)                       \
    V("Rar!\x1A\x07\x00", kindFileRar)     \
    V("Rar!\x1A\x07\x01\x00", kindFileRar) \
    V("7z\xBC\xAF\x27\x1C", kindFile7Z)    \
    V("PK\x03\x04", kindFileZip)           \
    V("ITSF", kindFileChm)                 \
    V("AT&T", kindFileDjVu)

struct FileSig {
    const char* sig;
    size_t sigLen;
    Kind kind;
};

#define MK_SIG(SIG, KIND) {SIG, sizeof(SIG) - 1, KIND},

static FileSig gFileSigs[] = {FILE_SIGS(MK_SIG)};

#undef MK_SIG

// detect file type based on file content
// we don't support sniffing kindFileVbkm
Kind SniffFileTypeFromData(std::span<u8> d) {
    if (IsPdfFileContent(d)) {
        return kindFilePDF;
    }
    if (IsPSFileContent(d)) {
        return kindFilePS;
    }
    // TODO: sniff .fb2 content
    u8* data = d.data();
    size_t len = d.size();
    ImgFormat fmt = GfxFormatFromData(d);
    switch (fmt) {
        case ImgFormat::BMP:
            return kindFileBmp;
        case ImgFormat::GIF:
            return kindFileGif;
        case ImgFormat::JPEG:
            return kindFileJpeg;
        case ImgFormat::JXR:
            return kindFileJxr;
        case ImgFormat::PNG:
            return kindFilePng;
        case ImgFormat::TGA:
            return kindFileTga;
        case ImgFormat::TIFF:
            return kindFileTiff;
        case ImgFormat::WebP:
            return kindFileWebp;
        case ImgFormat::JP2:
            return kindFileJp2;
    }
    int n = (int)dimof(gFileSigs);
    for (int i = 0; i < n; i++) {
        const char* sig = gFileSigs[i].sig;
        size_t sigLen = gFileSigs[i].sigLen;
        if (memeq(data, sig, sigLen)) {
            return gFileSigs[i].kind;
        }
    }
    return nullptr;
}

bool IsPSFileContent(std::span<u8> d) {
    char* header = (char*)d.data();
    size_t n = d.size();
    if (n < 64) {
        return false;
    }
    // Windows-format EPS file - cf. http://partners.adobe.com/public/developer/en/ps/5002.EPSF_Spec.pdf
    if (str::StartsWith(header, "\xC5\xD0\xD3\xC6")) {
        DWORD psStart = ByteReader(d).DWordLE(4);
        return psStart >= n - 12 || str::StartsWith(header + psStart, "%!PS-Adobe-");
    }
    if (str::StartsWith(header, "%!PS-Adobe-")) {
        return true;
    }
    // PJL (Printer Job Language) files containing Postscript data
    // https://developers.hp.com/system/files/PJL_Technical_Reference_Manual.pdf
    bool isPJL = str::StartsWith(header, "\x1B%-12345X@PJL");
    if (isPJL) {
        // TODO: use something else other than str::Find() so that it works even if header is not null-terminated
        const char* hdr = str::Find(header, "\n%!PS-Adobe-");
        if (!hdr) {
            isPJL = false;
        }
    }
    return isPJL;
}

bool IsEpubFile(const WCHAR* path) {
    AutoDelete<MultiFormatArchive> archive = OpenZipArchive(path, true);
    if (!archive.get()) {
        return false;
    }
    AutoFree mimetype(archive->GetFileDataByName("mimetype"));
    if (!mimetype.data) {
        return false;
    }
    char* d = mimetype.data;
    // trailing whitespace is allowed for the mimetype file
    for (size_t i = mimetype.size(); i > 0; i--) {
        if (!str::IsWs(d[i - 1])) {
            break;
        }
        d[i - 1] = '\0';
    }
    // a proper EPUB document has a "mimetype" file with content
    // "application/epub+zip" as the first entry in its ZIP structure
    /* cf. http://forums.fofou.org/sumatrapdf/topic?id=2599331
    if (!str::Eq(zip.GetFileName(0), L"mimetype"))
        return false; */
    if (str::Eq(mimetype.data, "application/epub+zip")) {
        return true;
    }
    // also open renamed .ibooks files
    // cf. http://en.wikipedia.org/wiki/IBooks#Formats
    return str::Eq(mimetype.data, "application/x-ibooks+zip");
}

bool IsMobiFile(const WCHAR* path) {
    PdbReader pdbReader;
    auto data = file::ReadFile(path);
    if (!pdbReader.Parse(data)) {
        return false;
    }
    // in most cases, we're only interested in Mobipocket files
    // (PalmDoc uses MobiDoc for loading other formats based on MOBI,
    // but implements sniffing itself in PalmDoc::IsSupportedFile)
    PdbDocType kind = GetPdbDocType(pdbReader.GetDbType());
    return PdbDocType::Mobipocket == kind;
}

// detect file type based on file content
Kind SniffFileType(const WCHAR* path) {
    CrashIf(!path);

    if (path::IsDirectory(path)) {
        AutoFreeWstr mimetypePath(path::Join(path, L"mimetype"));
        if (file::StartsWith(mimetypePath, "application/epub+zip")) {
            return kindFileEpub;
        }
        // TODO: check the content of directory for more types?
        return nullptr;
    }

    // +1 for zero-termination
    char buf[2048 + 1] = {0};
    int n = file::ReadN(path, buf, dimof(buf) - 1);
    if (n <= 0) {
        return nullptr;
    }
    auto res = SniffFileTypeFromData({(u8*)buf, (size_t)n});
    if (res == kindFileZip) {
        if (IsXpsArchive(path)) {
            res = kindFileXps;
        }
        if (IsEpubFile(path)) {
            res = kindFileEpub;
        }
    }
    if (!res) {
        if (IsMobiFile(path)) {
            res = kindFileMobi;
        }
    }
    return res;
}

Kind FileTypeFromFileName(const WCHAR* path) {
    VerifyExtsMatch();

    if (!path) {
        return nullptr;
    }
    if (path::IsDirectory(path)) {
        return kindFileDir;
    }
    Kind res = GetKindByFileExt(path);
    if (res != nullptr) {
        return res;
    }

    // those are cases that cannot be decided just by
    // looking at extension
    if (IsPdfFileName(path)) {
        return kindFilePDF;
    }

    return nullptr;
}
