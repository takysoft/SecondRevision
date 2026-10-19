// SecondRevision - reading a package's height map file: a zip (scripts/heightmap_pack.py) holding one DDS a texture and a
// header.txt naming them. Nothing here knows the engine or the card: bytes in, answers out, so the tests can reach
// every part of it (tests/RdxTests.cpp).
//
// The zip is read as its format describes it (PKWARE's APPNOTE): the end of the central directory at the file's end,
// the directory it points to, and a local header before each item's data. An item is either stored as it is or
// deflated (RFC 1951) - what every zip tool writes; the packing tool stores, as the maps are block compressed already,
// but a zip made by hand with compression is read too. The DDS is the one Microsoft documents: its 124-byte header,
// and the DX10 one after it when the format is named there.
#pragma once

namespace RdxArchive
{
inline unsigned Le16(const unsigned char* P)
{
    return (unsigned)P[0] | ((unsigned)P[1] << 8);
}

inline unsigned Le32(const unsigned char* P)
{
    return (unsigned)P[0] | ((unsigned)P[1] << 8) | ((unsigned)P[2] << 16) | ((unsigned)P[3] << 24);
}

// The end of the central directory, searched for from the end of the file's last Bytes (a comment may follow it, up
// to 64 KB): how many items, and where the directory is and how long.
inline bool ZipEnd(const unsigned char* Tail, int Bytes, unsigned& Items, unsigned& DirOffset, unsigned& DirBytes)
{
    for (int i = Bytes - 22; i >= 0; i--)
        if (Le32(Tail + i) == 0x06054b50u && i + 22 + (int)Le16(Tail + i + 20) <= Bytes)
        {
            Items     = Le16(Tail + i + 10);
            DirBytes  = Le32(Tail + i + 12);
            DirOffset = Le32(Tail + i + 16);
            return true;
        }
    return false;
}

// One item of the directory: its name, how it is packed (0 stored, 8 deflated), its size packed and unpacked, and
// where its local header starts.
struct FZipItem
{
    enum { NAME = 128 };
    char     Name[NAME];
    unsigned Method;
    unsigned Flags;
    unsigned Packed;
    unsigned Size;
    unsigned Local;
};

// The directory's items into Out, Max at most; how many were read, or -1 when the directory is not one. A name too
// long for FZipItem is cut short: such an item can never be one the header names.
inline int ZipItems(const unsigned char* Dir, int Bytes, FZipItem* Out, int Max)
{
    int Got = 0;
    for (int p = 0; p + 46 <= Bytes && Got < Max;)
    {
        if (Le32(Dir + p) != 0x02014b50u)
            return -1;
        const int NameBytes = (int)Le16(Dir + p + 28), Extra = (int)Le16(Dir + p + 30), Note = (int)Le16(Dir + p + 32);
        if (p + 46 + NameBytes + Extra + Note > Bytes)
            return -1;
        FZipItem& I = Out[Got++];
        I.Flags  = Le16(Dir + p + 8);
        I.Method = Le16(Dir + p + 10);
        I.Packed = Le32(Dir + p + 20);
        I.Size   = Le32(Dir + p + 24);
        I.Local  = Le32(Dir + p + 42);
        int n = 0;
        for (; n < NameBytes && n < FZipItem::NAME - 1; n++)
            I.Name[n] = (char)Dir[p + 46 + n];
        I.Name[n] = 0;
        p += 46 + NameBytes + Extra + Note;
    }
    return Got;
}

// Where an item's data starts, from the first 30 bytes of its local header.
inline bool ZipDataStart(const unsigned char* Local30, unsigned Local, unsigned& Start)
{
    if (Le32(Local30) != 0x04034b50u)
        return false;
    Start = Local + 30u + Le16(Local30 + 26) + Le16(Local30 + 28);
    return true;
}

// Deflate (RFC 1951): the stream In unpacked into exactly OutBytes of Out. False for anything that is not a whole,
// well-formed stream of that length - a bad file is passed over, never read past its end.
namespace Detail
{
    struct FBits
    {
        const unsigned char* In;
        int                  InBytes;
        int                  Pos;
        unsigned             Held;
        int                  Count;

        bool Take(int N, unsigned& V)
        {
            while (Count < N)
            {
                if (Pos >= InBytes)
                    return false;
                Held |= (unsigned)In[Pos++] << Count;
                Count += 8;
            }
            V = Held & ((1u << N) - 1u);
            Held >>= N;
            Count -= N;
            return true;
        }
    };

    // A canonical code (RFC 1951 3.2.2): how many codes of each length, and the symbols in code order.
    struct FCode
    {
        short Count[16];
        short Symbol[288];
    };

    // From each symbol's code length. False when the lengths ask for more codes than there are (a set with room left
    // over is allowed: a stream with one distance code has one).
    inline bool Make(FCode& C, const unsigned char* Lengths, int N)
    {
        for (int l = 0; l < 16; l++)
            C.Count[l] = 0;
        for (int s = 0; s < N; s++)
            C.Count[Lengths[s]]++;
        int Left = 1;
        for (int l = 1; l < 16; l++)
        {
            Left = Left * 2 - C.Count[l];
            if (Left < 0)
                return false;
        }
        short Next[16];
        Next[1] = 0;
        for (int l = 1; l < 15; l++)
            Next[l + 1] = (short)(Next[l] + C.Count[l]);
        for (int s = 0; s < N; s++)
            if (Lengths[s])
                C.Symbol[Next[Lengths[s]]++] = (short)s;
        return true;
    }

    // The next symbol: the code is read a bit at a time, most significant first, and compared with the first code of
    // each length in turn.
    inline bool Read(FBits& B, const FCode& C, int& Symbol)
    {
        int Code = 0, First = 0, Index = 0;
        for (int l = 1; l < 16; l++)
        {
            unsigned Bit;
            if (!B.Take(1, Bit))
                return false;
            Code |= (int)Bit;
            const int Count = C.Count[l];
            if (Code - First < Count)
            {
                Symbol = C.Symbol[Index + (Code - First)];
                return true;
            }
            Index += Count;
            First = (First + Count) << 1;
            Code <<= 1;
        }
        return false;
    }

    inline bool Pieces(FBits& B, const FCode& Lit, const FCode& Dist, unsigned char* Out, int OutBytes, int& At)
    {
        static const short LenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                           67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static const short LenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                            5, 5, 5, 5, 0 };
        static const short DistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                            769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
        static const short DistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                             11, 11, 12, 12, 13, 13 };
        for (;;)
        {
            int S;
            if (!Read(B, Lit, S))
                return false;
            if (S < 256)
            {
                if (At >= OutBytes)
                    return false;
                Out[At++] = (unsigned char)S;
                continue;
            }
            if (S == 256)
                return true;
            S -= 257;
            if (S >= 29)
                return false;
            unsigned X;
            if (!B.Take(LenExtra[S], X))
                return false;
            const int Len = LenBase[S] + (int)X;
            int D;
            if (!Read(B, Dist, D) || D >= 30 || !B.Take(DistExtra[D], X))
                return false;
            const int Back = DistBase[D] + (int)X;
            if (Back > At || At + Len > OutBytes)
                return false;
            for (int k = 0; k < Len; k++, At++)
                Out[At] = Out[At - Back];
        }
    }
}

inline bool Inflate(const unsigned char* In, int InBytes, unsigned char* Out, int OutBytes)
{
    Detail::FBits B = { In, InBytes, 0, 0, 0 };
    int           At = 0;
    for (;;)
    {
        unsigned Last, Type;
        if (!B.Take(1, Last) || !B.Take(2, Type))
            return false;
        if (Type == 0)
        {
            // Stored: from the next whole byte, its length and that length's complement, then the bytes as they are.
            B.Held  = 0;
            B.Count = 0;
            if (B.Pos + 4 > InBytes)
                return false;
            const int Len = (int)Le16(In + B.Pos), NotLen = (int)Le16(In + B.Pos + 2);
            B.Pos += 4;
            if ((Len ^ 0xffff) != NotLen || B.Pos + Len > InBytes || At + Len > OutBytes)
                return false;
            for (int k = 0; k < Len; k++)
                Out[At++] = In[B.Pos++];
        }
        else if (Type == 1)
        {
            // The fixed codes (RFC 1951 3.2.6).
            static Detail::FCode Lit, Dist;
            static bool          bMade = false;
            if (!bMade)
            {
                unsigned char L[288];
                int           s = 0;
                for (; s < 144; s++)
                    L[s] = 8;
                for (; s < 256; s++)
                    L[s] = 9;
                for (; s < 280; s++)
                    L[s] = 7;
                for (; s < 288; s++)
                    L[s] = 8;
                Detail::Make(Lit, L, 288);
                for (s = 0; s < 30; s++)
                    L[s] = 5;
                Detail::Make(Dist, L, 30);
                bMade = true;
            }
            if (!Detail::Pieces(B, Lit, Dist, Out, OutBytes, At))
                return false;
        }
        else if (Type == 2)
        {
            // The codes sent with the block (RFC 1951 3.2.7): the lengths of a code for the lengths, then the lengths.
            static const unsigned char Order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            unsigned NLit, NDist, NLen;
            if (!B.Take(5, NLit) || !B.Take(5, NDist) || !B.Take(4, NLen))
                return false;
            NLit += 257;
            NDist += 1;
            NLen += 4;
            if (NLit > 286 || NDist > 30)
                return false;
            unsigned char Lengths[320];
            for (int k = 0; k < 19; k++)
                Lengths[k] = 0;
            for (unsigned k = 0; k < NLen; k++)
            {
                unsigned V;
                if (!B.Take(3, V))
                    return false;
                Lengths[Order[k]] = (unsigned char)V;
            }
            Detail::FCode LenCode;
            if (!Detail::Make(LenCode, Lengths, 19))
                return false;
            unsigned char All[320];
            for (unsigned k = 0; k < NLit + NDist;)
            {
                int S;
                if (!Detail::Read(B, LenCode, S))
                    return false;
                if (S < 16)
                {
                    All[k++] = (unsigned char)S;
                    continue;
                }
                unsigned      Repeat, V;
                unsigned char What = 0;
                if (S == 16)
                {
                    if (k == 0 || !B.Take(2, V))
                        return false;
                    What   = All[k - 1];
                    Repeat = 3 + V;
                }
                else if (S == 17)
                {
                    if (!B.Take(3, V))
                        return false;
                    Repeat = 3 + V;
                }
                else
                {
                    if (!B.Take(7, V))
                        return false;
                    Repeat = 11 + V;
                }
                if (k + Repeat > NLit + NDist)
                    return false;
                while (Repeat--)
                    All[k++] = What;
            }
            if (All[256] == 0)
                return false;                       // a block with no way to end
            Detail::FCode Lit, Dist;
            if (!Detail::Make(Lit, All, (int)NLit) || !Detail::Make(Dist, All + NLit, (int)NDist))
                return false;
            if (!Detail::Pieces(B, Lit, Dist, Out, OutBytes, At))
                return false;
        }
        else
            return false;
        if (Last)
            return At == OutBytes;
    }
}

// A DDS holding a one-channel block compressed map (BC4): its size, how many halvings it carries (the full size
// counted), and where its data starts. The format named in the DX10 header (BC4_UNORM or its typeless form), or by
// either of the old four-character codes the tools write for it (ATI1, BC4U).
inline bool DdsBc4(const unsigned char* D, int Bytes, int& W, int& H, int& Mips, int& Start)
{
    if (Bytes < 128 || D[0] != 'D' || D[1] != 'D' || D[2] != 'S' || D[3] != ' ' || Le32(D + 4) != 124)
        return false;
    H    = (int)Le32(D + 12);
    W    = (int)Le32(D + 16);
    Mips = (Le32(D + 8) & 0x20000u) ? (int)Le32(D + 28) : 1;
    if (Mips < 1)
        Mips = 1;
    const unsigned PixelFlags = Le32(D + 80), FourCC = Le32(D + 84);
    if (!(PixelFlags & 0x4u))
        return false;                               // no four-character code: an uncompressed form
    if (FourCC == 0x31495441u || FourCC == 0x55344342u)       // 'ATI1', 'BC4U'
        Start = 128;
    else if (FourCC == 0x30315844u)                            // 'DX10'
    {
        if (Bytes < 148)
            return false;
        const unsigned Format = Le32(D + 128);
        if (Format != 80u && Format != 79u)                    // DXGI_FORMAT_BC4_UNORM, BC4_TYPELESS
            return false;
        Start = 148;
    }
    else
        return false;
    return W > 0 && H > 0 && W <= 16384 && H <= 16384;
}

// How many bytes a BC4 map of W x H takes with Mips halvings, the full size counted: eight a 4x4 block.
inline long long Bc4Bytes(int W, int H, int Mips)
{
    long long Total = 0;
    for (int m = 0; m < Mips; m++)
    {
        Total += (long long)((W + 3) / 4) * ((H + 3) / 4) * 8;
        W = (W > 1) ? W / 2 : 1;
        H = (H > 1) ? H / 2 : 1;
    }
    return Total;
}
}
