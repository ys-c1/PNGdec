//
// PNG Decoder
//
// written by Larry Bank
// bitbank@pobox.com
// Arduino port started 5/3/2021
// Original PNG code written 20+ years ago :)
// The goal of this code is to decode PNG images on embedded systems
//
// Copyright 2021 BitBank Software, Inc. All Rights Reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//===========================================================================
//
#ifndef __PNGDEC__
#define __PNGDEC__
#if defined( __MACH__ ) || defined( __LINUX__ ) || defined( __MCUXPRESSO )
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#define memcpy_P memcpy
#define PROGMEM
#else
#include <Arduino.h>
#endif
#include "zutil.h"
#include "inftrees.h"
#include "inflate.h"
//
// PNG Decoder
// Written by Larry Bank
// Copyright (c) 2021 BitBank Software, Inc.
// 
// Designed to decode most PNG images (1-32 bpp)
// using less than 40K of RAM
//
#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif
/* Defines and variables */
#define PNG_FILE_BUF_SIZE 2048
// Number of bytes to reserve for current and previous lines
// Defaults to 320 32-bit pixels max width
// but can be overidden with a macro defined at compile time
#ifndef PNG_MAX_BUFFERED_PIXELS
#if defined( __LINUX__ ) || defined( __MACH__ )
#define PNG_MAX_BUFFERED_PIXELS 16386
#else
#define PNG_MAX_BUFFERED_PIXELS ((320*4 + 1)*2)
#endif
#endif
#ifndef PNG_COMMENT_SIZE
#define PNG_COMMENT_SIZE 128
#endif // PNG_COMMENT_SIZE

#ifndef __PNGENC__
// PNG filter type
enum {
    PNG_FILTER_NONE=0,
    PNG_FILTER_SUB,
    PNG_FILTER_UP,
    PNG_FILTER_AVG,
    PNG_FILTER_PAETH,
    PNG_FILTER_COUNT
};

// decode options
enum {
    PNG_CHECK_CRC = 1,
    PNG_FAST_PALETTE = 2
};

// source pixel type
enum {
  PNG_PIXEL_GRAYSCALE=0,
    PNG_PIXEL_TRUECOLOR=2,
    PNG_PIXEL_INDEXED=3,
    PNG_PIXEL_GRAY_ALPHA=4,
    PNG_PIXEL_TRUECOLOR_ALPHA=6
};

// Error codes returned by getLastError()
enum {
    PNG_SUCCESS = 0,
    PNG_INVALID_PARAMETER,
    PNG_DECODE_ERROR,
    PNG_MEM_ERROR,
    PNG_NO_BUFFER,
    PNG_UNSUPPORTED_FEATURE,
    PNG_INVALID_FILE,
    PNG_TOO_BIG,
    PNG_QUIT_EARLY
};

typedef struct png_file_tag
{
  int32_t iPos; // current file position
  int32_t iSize; // file size
  uint8_t *pData; // memory file pointer
  void * fHandle; // class pointer to File/SdFat or whatever you want
} PNGFILE;

#endif // !__PNGENC__

// RGB565 endianness
enum {
    PNG_RGB565_LITTLE_ENDIAN = 0,
    PNG_RGB565_BIG_ENDIAN
};

enum {
    PNG_MEM_RAM=0,
    PNG_MEM_FLASH
};

typedef struct png_draw_tag
{
    int y; // starting x,y of this line
    int iWidth; // size of this line
    int iPitch; // bytes per line
    int iPixelType; // PNG pixel type (0,2,3,4,6)
    int iBpp; // bits per color stimulus
    int iHasAlpha; // flag indicating the presence of an alpha palette
    void *pUser; // user supplied pointer
    uint8_t *pPalette;
    uint16_t *pFastPalette;
    uint8_t *pPixels;
} PNGDRAW;

//
// Row view support (optional, opt-in via enableRowView)
//
// One read-only descriptor per decoded image line. pData points at the
// first valid pixel byte of the line and iLen is the number of valid
// bytes (equal to the PNG image pitch). The backing memory is owned by
// the PNG object and is released by disableRowView()/the destructor,
// after which every descriptor must be considered invalid.
typedef struct png_row_tag
{
    const uint8_t *pData; // start of valid row pixels
    int iLen;             // number of valid bytes (== image pitch)
} PNGROW;

// Row view options for PNGROWCONFIG.iOptions
enum {
    // Defer RGB565 conversion until getRowRGB565() is called (only the
    // most recently requested row is kept). Without this flag every row
    // is converted eagerly while the image is decoded.
    PNG_ROW_LAZY_RGB565 = 1
};

typedef struct png_row_config_tag
{
    int iAlign;   // alignment of each row start: 1, 2, 4, 8 or 16 (0 => 1)
    int iOptions; // combination of PNG_ROW_* option flags
} PNGROWCONFIG;

// Callback function prototypes
typedef int32_t (PNG_READ_CALLBACK)(PNGFILE *pFile, uint8_t *pBuf, int32_t iLen);
typedef int32_t (PNG_SEEK_CALLBACK)(PNGFILE *pFile, int32_t iPosition);
typedef void * (PNG_OPEN_CALLBACK)(const char *szFilename, int32_t *pFileSize);
typedef int (PNG_DRAW_CALLBACK)(PNGDRAW *);
typedef void (PNG_CLOSE_CALLBACK)(void *pHandle);

//
// our private structure to hold a JPEG image decode state
//
typedef struct png_image_tag
{
    int iWidth, iHeight; // image size
    uint8_t ucBpp, ucPixelType;
    uint8_t ucMemType;
    uint8_t *pImage;
    int iPitch; // bytes per line
    int iHasAlpha;
    int iInterlaced;
    uint32_t iTransparent; // transparent color index/value
    int iError;
    PNG_READ_CALLBACK *pfnRead;
    PNG_SEEK_CALLBACK *pfnSeek;
    PNG_OPEN_CALLBACK *pfnOpen;
    PNG_DRAW_CALLBACK *pfnDraw;
    PNG_CLOSE_CALLBACK *pfnClose;
    PNGFILE PNGFile;
    uint8_t ucZLIB[32768 + sizeof(struct inflate_state)]; // put this here to avoid needing malloc/free
    uint8_t ucPalette[1024];
    uint8_t ucPixels[PNG_MAX_BUFFERED_PIXELS];
    uint8_t ucFileBuf[PNG_FILE_BUF_SIZE]; // holds temp file data
    char szComment[PNG_COMMENT_SIZE];
} PNGIMAGE;

#ifdef __cplusplus
#define PNG_STATIC static
// Row-view state is defined later in this header together with its methods.
struct PNGRowViewState;
//
// The PNG class wraps portable C code which does the actual work
//
class PNG
{
  public:
    ~PNG();
    int openRAM(uint8_t *pData, int iDataSize, PNG_DRAW_CALLBACK *pfnDraw);
    int openFLASH(uint8_t *pData, int iDataSize, PNG_DRAW_CALLBACK *pfnDraw);
    int open(const char *szFilename, PNG_OPEN_CALLBACK *pfnOpen, PNG_CLOSE_CALLBACK *pfnClose, PNG_READ_CALLBACK *pfnRead, PNG_SEEK_CALLBACK *pfnSeek, PNG_DRAW_CALLBACK *pfnDraw);
    void close();
    int decode(void *pUser, int iOptions);
    int getWidth();
    int getHeight();
    int getBpp();
    int hasAlpha();
    uint32_t getTransparentColor();
    int isInterlaced();
    uint8_t * getPalette();
    int getPixelType();
    int getLastError();
    int getBufferSize();
    char *getComment();
    uint8_t *getBuffer();
    void setBuffer(uint8_t *pBuffer);
    uint8_t getAlphaMask(PNGDRAW *pDraw, uint8_t *pMask, uint8_t ucThreshold);
    void getLineAsRGB565(PNGDRAW *pDraw, uint16_t *pPixels, int iEndianness, uint32_t u32Bkgd);
    //
    // Optional read-only row view
    //
    // enableRowView() must be called after open*() and before decode().
    // It switches this decode to the line-by-line path and records, for
    // every output line, its start pointer and valid byte count.
    // iAlign must be 0/1/2/4/8/16 (0 means byte packed); iOptions can
    // contain PNG_ROW_LAZY_RGB565 to defer RGB565 conversion until a row
    // is requested. Returns PNG_SUCCESS, PNG_INVALID_PARAMETER or
    // PNG_MEM_ERROR. Re-decoding is allowed; the view is rebuilt each time.
    // disableRowView() releases the row memory; afterwards the row array
    // is invalid and getRows() returns NULL. Call it before re-opening the
    // object on another file.
    int enableRowView(PNGROWCONFIG *pConfig);
    void disableRowView(void);
    int isRowViewValid(void);
    const PNGROW *getRows(int *pRowCount);
    int getRowViewMemory(void);          // total bytes owned by the view
    int getRowViewConvertedRows(void);   // RGB565 rows materialized so far
    // Returns a read-only RGB565 view of one row (lazy conversion when
    // enabled), or NULL if the view is invalid or the row index is out of
    // range. The returned pointer belongs to the PNG object and shares the
    // view's lifetime.
    const uint16_t *getRowRGB565(int iRow, int iEndianness, uint32_t u32Bkgd);

  private:
    PNGRowViewState *rowViewState(void) const;
    PNGIMAGE _png;
};

//===========================================================================
// Row view implementation (header-only so no other library file changes)
//
// All view memory lives in one heap block laid out as:
//   [PNGRowViewState][PNGROW array][aligned raw rows][RGB565 storage]
// so enable does one malloc() and disable does one free(). In lazy mode
// the RGB565 storage is a single reusable row cache; in eager mode it
// holds the converted whole image.
//===========================================================================
#define PNG_ROW_VIEW_MAGIC 0x52565731 // 'RVW1'

struct PNGRowViewState
{
    uint32_t u32Magic;
    PNG *pOwner;
    PNGRowViewState *pNext; // intrusive registry chain (survives open* memset)
    int iAlign;        // normalized alignment (1,2,4,8,16)
    int iPitch;        // valid bytes per row
    int iRowStride;    // bytes between row starts inside the block
    int iWidth;
    int iHeight;
    int iConvWidth;    // RGB565 pixels written per row (rounded up for
                       // sub-byte source formats; see PNGRGB565)
    int iCount;        // rows recorded by the most recent decode
    int iOptions;
    int iTotalMem;     // size of the single heap block
    int iConverted;    // distinct RGB565 rows materialized
    int iLazy;         // non-zero when PNG_ROW_LAZY_RGB565 is set
    int iCachedRow;    // row currently held in the lazy cache (-1 = none)
    int iCachedEndian;
    uint32_t u32CachedBkgd;
    PNGROW *pRows;     // iHeight descriptors (inside the block)
    uint8_t *pPixels;  // aligned raw row storage (inside the block)
    uint16_t *pConv;   // RGB565 storage (inside the block)
    uint8_t *pLineBufBase; // owner's internal ucPixels range, used by the
    size_t zLineBufSize;   // draw thunk to identify the owning object
};

// The registry head tracks every live block so that re-opening a PNG
// object (which memsets its PNGIMAGE and would otherwise orphan the
// block) cannot leak memory. The draw thunk resolves its owning state by
// checking which object's internal line buffer the descriptor points at,
// so several enabled objects never confuse each other.
static PNGRowViewState *g_pPNGRowViewHead = NULL;

// Free every row-view block owned by this PNG object and unlink it.
static inline void PNGRowViewReleaseOwner(PNG *pOwner)
{
    PNGRowViewState **pp = &g_pPNGRowViewHead;
    while (*pp != NULL) {
        PNGRowViewState *pState = *pp;
        if (pState->pOwner == pOwner) {
            *pp = pState->pNext; // unlink
            free(pState); // single block; the state header is its first member
        } else {
            pp = &pState->pNext;
        }
    }
}

static inline size_t PNGRowViewAlignUp(size_t x, size_t a)
{
    return (x + a - 1) & ~(a - 1);
}

static inline int PNGRowViewThunk(PNGDRAW *pDraw)
{
    PNGRowViewState *pState, *pTarget = NULL;
    int y = pDraw->y;
    // Locate the enabled object whose internal line buffer produced this
    // line, so multiple enabled PNG objects cannot confuse each other.
    for (pState = g_pPNGRowViewHead; pState != NULL; pState = pState->pNext) {
        if (pDraw->pPixels >= pState->pLineBufBase &&
            pDraw->pPixels < pState->pLineBufBase + pState->zLineBufSize) {
            pTarget = pState;
            break;
        }
    }
    pState = pTarget;
    if (pState == NULL || y < 0 || y >= pState->iHeight)
        return 0; // abort on anything unexpected
    if (y == 0) { // a fresh decode starts here; reset per-decode counters
        pState->iCount = 0;
        pState->iConverted = 0;
    }
    uint8_t *pDst = pState->pPixels + (size_t)y * pState->iRowStride;
    memcpy(pDst, pDraw->pPixels, pState->iPitch);
    pState->pRows[y].pData = pDst;
    pState->pRows[y].iLen = pState->iPitch;
    pState->iCount = y + 1;
    if (!pState->iLazy) {
        // Eager mode: convert this line immediately while palette/type info
        // is still available through the draw descriptor.
        pState->pOwner->getLineAsRGB565(pDraw,
                                        &pState->pConv[(size_t)y * pState->iConvWidth],
                                        PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
        pState->iConverted = y + 1;
    }
    return 1; // continue decoding
}

inline PNG::~PNG()
{
    disableRowView();
}

inline int PNG::enableRowView(PNGROWCONFIG *pConfig)
{
    int iAlign, iOptions, iLazy, iRowStride;
    size_t zOff, zBlockSize;
    uint8_t *pBlock;
    PNGRowViewState *pState;

    if (pConfig == NULL || _png.iWidth <= 0 || _png.iHeight <= 0)
        return PNG_INVALID_PARAMETER;
    if (_png.iError != PNG_SUCCESS)
        return _png.iError; // e.g. unsupported 16-bit/interlaced file
    iAlign = pConfig->iAlign ? pConfig->iAlign : 1;
    if (iAlign != 1 && iAlign != 2 && iAlign != 4 && iAlign != 8 && iAlign != 16)
        return PNG_INVALID_PARAMETER;
    if (pConfig->iOptions & ~(PNG_ROW_LAZY_RGB565))
        return PNG_INVALID_PARAMETER;
    iOptions = pConfig->iOptions;
    iLazy = (iOptions & PNG_ROW_LAZY_RGB565) ? 1 : 0;

    PNGRowViewReleaseOwner(this); // release any view this object still owns

    iRowStride = (int)PNGRowViewAlignUp((size_t)_png.iPitch, (size_t)iAlign);
    // PNGRGB565 processes sub-byte pixels in whole-byte groups, so odd
    // widths produce one extra RGB565 pixel on 1/2/4-bpp sources. Size
    // the conversion storage for the rounded-up pixel count.
    int iConvWidth = _png.iWidth;
    if (_png.ucBpp < 8 &&
        (_png.ucPixelType == PNG_PIXEL_INDEXED ||
         _png.ucPixelType == PNG_PIXEL_GRAYSCALE)) {
        int group = 8 / _png.ucBpp; // pixels written per source byte
        iConvWidth = ((_png.iWidth + group - 1) / group) * group;
    }
    // Layout: state header, descriptor array, aligned raw rows, RGB565.
    zOff = PNGRowViewAlignUp(sizeof(PNGRowViewState), sizeof(void *));
    zOff += sizeof(PNGROW) * (size_t)_png.iHeight;
    zOff = PNGRowViewAlignUp(zOff, (size_t)iAlign);
    zOff += (size_t)iRowStride * _png.iHeight;
    zOff = PNGRowViewAlignUp(zOff, 2);
    zOff += (size_t)iConvWidth * 2 * (iLazy ? 1 : _png.iHeight);
    zBlockSize = zOff;

    pBlock = (uint8_t *)malloc(zBlockSize);
    if (pBlock == NULL)
        return PNG_MEM_ERROR;
    memset(pBlock, 0, zBlockSize);
    pState = (PNGRowViewState *)pBlock; // state is always first in the block
    pState->u32Magic = PNG_ROW_VIEW_MAGIC;
    pState->pOwner = this;
    pState->pNext = g_pPNGRowViewHead; // register before anyone can use it
    g_pPNGRowViewHead = pState;
    pState->iAlign = iAlign;
    pState->iOptions = iOptions;
    pState->iLazy = iLazy;
    pState->iPitch = _png.iPitch;
    pState->iRowStride = iRowStride;
    pState->iWidth = _png.iWidth;
    pState->iHeight = _png.iHeight;
    pState->iConvWidth = iConvWidth;
    pState->iCount = 0;
    pState->iConverted = 0;
    pState->iCachedRow = -1;
    pState->iTotalMem = (int)zBlockSize;

    zOff = PNGRowViewAlignUp(sizeof(PNGRowViewState), sizeof(void *));
    pState->pRows = (PNGROW *)(pBlock + zOff);
    zOff += sizeof(PNGROW) * (size_t)_png.iHeight;
    zOff = PNGRowViewAlignUp(zOff, (size_t)iAlign);
    pState->pPixels = pBlock + zOff;
    zOff += (size_t)iRowStride * _png.iHeight;
    zOff = PNGRowViewAlignUp(zOff, 2);
    pState->pConv = (uint16_t *)(pBlock + zOff);
    pState->pLineBufBase = &_png.ucPixels[0];
    pState->zLineBufSize = sizeof(_png.ucPixels);

    // Force the line-by-line path; the thunk records each line.
    _png.pImage = NULL;
    _png.pfnDraw = PNGRowViewThunk;
    return PNG_SUCCESS;
}

inline void PNG::disableRowView(void)
{
    // Releasing by owner is memset-proof: open*() may have zeroed the
    // PNGIMAGE member, but the registry still finds the block.
    PNGRowViewReleaseOwner(this);
    // Restore the default full-image behavior for any later decode.
    _png.pfnDraw = NULL;
    _png.pImage = NULL;
}

// Resolve this object's current view block from the global registry.
inline PNGRowViewState *PNG::rowViewState(void) const
{
    PNGRowViewState *pState = g_pPNGRowViewHead;
    while (pState != NULL) {
        if (pState->pOwner == this &&
            pState->u32Magic == PNG_ROW_VIEW_MAGIC)
            return pState;
        pState = pState->pNext;
    }
    return NULL;
}

inline int PNG::isRowViewValid(void)
{
    PNGRowViewState *pState = rowViewState();
    int i;
    if (pState == NULL)
        return FALSE;
    // A failed decode (including one that produced no line at all) leaves
    // iError set, so stale row data from an earlier decode can't pass.
    if (_png.iError != PNG_SUCCESS)
        return FALSE;
    if (pState->iCount != pState->iHeight)
        return FALSE; // decode never completed all rows
    for (i = 0; i < pState->iHeight; i++) {
        if (pState->pRows[i].pData == NULL || pState->pRows[i].iLen != pState->iPitch)
            return FALSE;
    }
    return TRUE;
}

inline const PNGROW *PNG::getRows(int *pRowCount)
{
    PNGRowViewState *pState = rowViewState();
    if (pRowCount)
        *pRowCount = 0;
    if (!isRowViewValid())
        return NULL;
    if (pRowCount)
        *pRowCount = pState->iCount;
    return pState->pRows;
}

inline int PNG::getRowViewMemory(void)
{
    PNGRowViewState *pState = rowViewState();
    if (pState == NULL)
        return 0;
    return pState->iTotalMem;
}

inline int PNG::getRowViewConvertedRows(void)
{
    PNGRowViewState *pState = rowViewState();
    if (pState == NULL)
        return 0;
    return pState->iConverted;
}

inline const uint16_t *PNG::getRowRGB565(int iRow, int iEndianness, uint32_t u32Bkgd)
{
    PNGRowViewState *pState = rowViewState();
    PNGDRAW d;
    if (!isRowViewValid() || iRow < 0 || iRow >= pState->iHeight)
        return NULL;
    if (!pState->iLazy) {
        // Eager views expose the canonical conversion done during decode.
        if (iEndianness != PNG_RGB565_LITTLE_ENDIAN || u32Bkgd != 0xffffffff)
            return NULL;
        return &pState->pConv[(size_t)iRow * pState->iConvWidth];
    }
    // Lazy mode: one reusable cache, re-converted only when the requested
    // row or conversion parameters change.
    if (pState->iCachedRow == iRow && pState->iCachedEndian == iEndianness &&
        pState->u32CachedBkgd == u32Bkgd) {
        return pState->pConv;
    }
    memset(&d, 0, sizeof(d));
    d.y = iRow;
    d.iWidth = pState->iWidth;
    d.iPitch = pState->iPitch;
    d.iPixelType = _png.ucPixelType;
    d.iBpp = _png.ucBpp;
    d.iHasAlpha = _png.iHasAlpha;
    d.pPalette = _png.ucPalette;
    d.pPixels = (uint8_t *)pState->pRows[iRow].pData;
    getLineAsRGB565(&d, pState->pConv, iEndianness, u32Bkgd);
    if (pState->iCachedRow != iRow)
        pState->iConverted++;
    pState->iCachedRow = iRow;
    pState->iCachedEndian = iEndianness;
    pState->u32CachedBkgd = u32Bkgd;
    return pState->pConv;
}
#else
#define PNG_STATIC
int PNG_openRAM(PNGIMAGE *pPNG, uint8_t *pData, int iDataSize, PNG_DRAW_CALLBACK *pfnDraw);
int PNG_openFile(PNGIMAGE *pPNG, const char *szFilename, PNG_DRAW_CALLBACK *pfnDraw);
int PNG_getWidth(PNGIMAGE *pPNG);
int PNG_getHeight(PNGIMAGE *pPNG);
int PNG_decode(PNGIMAGE *pPNG, void *pUser, int iOptions);
void PNG_close(PNGIMAGE *pPNG);
int PNG_getLastError(PNGIMAGE *pPNG);
int PNG_getBpp(PNGIMAGE *pPNG);
int PNG_getLastError(PNGIMAGE *pPNG);
int PNG_getBufferSize(PNGIMAGE *pPNG);
uint8_t *PNG_getPalette(PNGIMAGE *pPNG);
int PNG_getPixelType(PNGIMAGE *pPNG);
int PNG_hasAlpha(PNGIMAGE *pPNG);
int PNG_isInterlaced(PNGIMAGE *pPNG);
uint8_t *PNG_getBuffer(PNGIMAGE *pPNG);
void PNG_setBuffer(PNGIMAGE *pPNG, uint8_t *pBuffer);
#endif // __cplusplus

// Due to unaligned memory causing an exception, we have to do these macros the slow way
#ifndef MOTOLONG
#define INTELSHORT(p) ((*p) + (*(p+1)<<8))
#define INTELLONG(p) ((*p) + (*(p+1)<<8) + (*(p+2)<<16) + (*(p+3)<<24))
#define MOTOSHORT(p) (((*(p))<<8) + (*(p+1)))
#define MOTOLONG(p) (((*p)<<24) + ((*(p+1))<<16) + ((*(p+2))<<8) + (*(p+3)))
#endif

// Must be a 32-bit target processor
#define REGISTER_WIDTH 32

#endif // __PNGDEC__
