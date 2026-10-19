// SecondRevision - the height maps the relief on the walls is read from (POM).
//
// A texture package's maps sit in a file beside it: <Package>.heightmap next to <Package>.utx, in whichever folder of
// the game that package is loaded from. The engine is asked where it loads a package from (appFindPackageFile - the
// lookup it loads packages with), so every copy of a package - the original game's, a replacement's, a mod's - brings
// the maps made for ITS pictures, and nothing here knows the game's search order. The file is a zip (PomArchive.h):
// one DDS a texture and <Package>.heightmap.txt - a line a texture: its name, its file, how deep its relief goes
// (scripts/heightmap_pack.py). A <Package>.heightmap.txt lying beside the .heightmap is read instead of the one inside
// it: a depth changed in play is written there, so nobody has to edit a zip.
//
// Nothing here is required. A texture with no file of its own has no entry, and the surface then draws exactly as it
// always did; a whole set that is not on disk simply has no entries at all. That is what lets this ship before the
// maps do.
#pragma once

// Only the cache's NAME is needed here: it hands out the descriptor slots these maps are written into, and the
// header itself is not one that can be included on its own (it leans on Backend.h).
class FTextureCache;

class FPomLibrary
{
public:
    FPomLibrary();
    ~FPomLibrary();

    UBOOL Init(ID3D12Device* InDevice);
    void  Shutdown();

    // Starts reading the files beside the packages - the extension, as the packing tool writes it (lower case). A
    // package's file is opened the first time one of its textures is asked about; nothing reaches the card until a
    // surface asks for it.
    void  SetMethod(const char* Extension);
    const char* GetMethod() const { return Method; }

    // How many maps the packages opened so far hold - what the readout reports, so a player can see at a glance
    // whether there are any.
    INT   GetCount() const { return Count; }
    INT   GetUploaded() const { return Uploaded; }

    // The entry for a texture, or -1. The key is the name the game calls it by - its package and its own name - looked
    // up in the file beside the package the engine loads under that name, which is opened the first time.
    INT   Find(const TCHAR* PathName, INT USize, INT VSize);

    // Reads the file and puts it on the card, the first time a surface asks. Returns false while the entry has no
    // maps - the caller then draws the surface as it always did.
    UBOOL Prepare(INT Entry, ID3D12GraphicsCommandList* Cmd, FTextureCache& Textures);

    // Whether THIS texture's relief is on, and how deep it goes - its package's list says. A depth of 0 is off;
    // switched off it goes to 0, switched on again to 1.
    UBOOL GetOn(INT Entry) const;
    FLOAT GetDepth(INT Entry) const;
    void  SetOn(INT Entry, UBOOL bOn);
    void  SetDepth(INT Entry, FLOAT Depth);
    // The lists of the packages changed in play back to disk, each beside its package: <Package>.heightmap.txt.
    UBOOL WriteList();
    const char* GetKey(INT Entry) const;

    // The same lookup, but remembering what a texture resolved to. The geometry pass asks once a SURFACE, and a
    // level draws the same texture over and over; without this every one of them builds the engine's path name
    // (which walks the object's outers and allocates) and walks the whole list of maps.
    // Remembered(): the answer if this texture has been asked about before, PomNotAsked otherwise. Split from
    // FindCached so the caller only pays for the engine's path name on the first sight of a texture.
    enum { PomNotAsked = -3 };
    INT   Remembered(QWORD CacheId, INT USize, INT VSize) const;
    INT   FindCached(QWORD CacheId, const TCHAR* PathName, INT USize, INT VSize);

    INT   GetHeightSrv(INT Entry) const;
    // One over the MAP's own size, which the shader needs to step through it a texel at a time. A smaller set's map is
    // smaller than the texture it belongs to.
    void  GetSize(INT Entry, FLOAT& OutU, FLOAT& OutV) const;

    // Staging buffers must outlive the copy, so they are let go only once the GPU has passed the frame that used
    // them. Called with the slot the frame about to be recorded will use: whatever is still tagged with that slot was
    // put there a whole round of frames ago, and the backend has waited for it.
    void  ReleaseStaging(INT FrameIndex);

private:
    // MAX_ENTRIES: one set can hold a map for every texture picked for the relief. FTextureCache's POM_VIEWS has room
    // for two descriptors to each, and the two the last one's table reads past its own.
    enum { MAX_ENTRIES = 2048, MAX_KEY = 96, MAX_MIPS = 14, MAX_PACKS = 512, MAX_PACK_NAME = 64, MAX_FILE = 128 };

    struct FEntry
    {
        char            Key[MAX_KEY];     // package and name, lower case, as it is looked up
        char            Name[MAX_KEY];    // and as the list spells them, to write the list back
        char            File[MAX_FILE];   // the item in the zip that holds its map
        INT             Pack;             // the package file it is in (Packs)
        UINT            Local;            // where its item's local header starts in that file
        UINT            Packed;           // and the item's bytes as they lie there, and unpacked
        UINT            Size;
        UINT            ZipMethod;        // 0 stored, 8 deflated
        INT             MapU;             // the map's own size, which is smaller than the texture's in a smaller set
        INT             MapV;
        UBOOL           bOn;              // the player's own choice for this texture
        FLOAT           Depth;            // how deep its relief goes, in the level's units
        UBOOL           bTried;           // read once; a file that failed is not read again every frame
        ID3D12Resource* Height;
        INT             HeightSrv;
        INT             Mips;             // how many shrunken copies the map has
    };

    UBOOL Upload(FEntry& E, INT Index, const BYTE* Data, INT Bytes, ID3D12GraphicsCommandList* Cmd,
                 FTextureCache& Textures);
    // The shader reads the relief through the water's table, and that table asks for FOUR descriptors from where it
    // is pointed - the height, and three the relief never reads: the second of its own two slots, and two that belong
    // to the next map along, which may not exist yet. A table with a descriptor that was never written is undefined
    // behaviour, so every slot in the region gets a real view of one tiny resource first, and a map then writes over
    // the first of its own two.
    void  EnsureBlank(FTextureCache& Textures);
    // The package a path name belongs to, its file opened and its entries made the first time: its index in Packs, or
    // -1 when there is no room for another.
    INT   OpenPack(const TCHAR* PathName);
    void  ReadPack(INT Index, const WCHAR* Path);
    // One item of a package file, unpacked into memory the caller frees (appFree).
    UBOOL ReadItem(HANDLE F, UINT Local, UINT Packed, UINT Size, UINT ZipMethod, BYTE*& Data, INT& Bytes) const;
    // One package's list back beside it, as it stands.
    UBOOL WritePack(INT Index);
    UBOOL WriteListLine(HANDLE F, const FEntry& E);

    ID3D12Device*   Device;
    FEntry          Entries[MAX_ENTRIES];
    INT             Count;
    INT             Uploaded;
    // Frames still to pass after a set change before anything is uploaded: the new maps go into the same descriptor
    // slots, and a frame recorded before the change may still be reading them on the card.
    INT             Hold;
    char            Method[32];
    // The packages asked about: by name, lower case, and the file beside the one the engine loads (empty for none).
    struct FPack
    {
        char  Name[MAX_PACK_NAME];
        WCHAR Path[MAX_PATH + 32];
        UBOOL bChanged;                   // a depth of it was changed in play and not written yet
        UBOOL bSaidUnwritable;            // the log said once that its list could not be written
    };
    FPack           Packs[MAX_PACKS];
    INT             PackCount;
    UBOOL           bSaidFull;            // the log said once that there is no room for more entries
    // What is waiting for the GPU: the upload buffers of the frames still in flight, and the resources a set change
    // left behind. Each carries the frame slot it was made in, and goes when that slot comes round again. A set change
    // retires up to two resources an entry, and every one of them must fit: one that does not would be let go while a
    // frame in flight may still be reading it. The margin is for a second change before the first has gone: what the
    // new set managed to upload in between, at most MAX_STAGING entries a frame, two resources each.
    enum { MAX_STAGING = 32, MAX_RETIRED = 2 * MAX_ENTRIES + 8 * MAX_STAGING };
    ID3D12Resource* Staging[MAX_STAGING];
    INT             StagingFrame[MAX_STAGING];
    INT             StagingCount;
    ID3D12Resource* Retired[MAX_RETIRED];
    INT             RetiredFrame[MAX_RETIRED];
    INT             RetiredCount;
    INT             CurrentFrame;         // the slot the frame being recorded uses
    ID3D12Resource* Blank;                // see EnsureBlank
    // What a texture resolved to last time, by the engine's own id for it. Direct mapped: a miss simply costs what
    // the lookup always cost. Cleared whenever the set of maps changes.
    enum { MEMO_SLOTS = 256 };
    QWORD           MemoId[MEMO_SLOTS];
    INT             MemoU[MEMO_SLOTS];
    INT             MemoV[MEMO_SLOTS];
    INT             MemoEntry[MEMO_SLOTS];
    UBOOL           MemoUsed[MEMO_SLOTS];
    UBOOL           HasStagingRoom() const { return StagingCount < MAX_STAGING; }
};
