/*
 * Help Viewer
 *
 * Copyright    1996 Ulrich Schmid
 *              2002 Eric Pouech
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

struct tagHelpFile;

typedef struct
{
    LPCSTR                      lpszPath;
    LONG                        lHash;
    BOOL                        bPopup;
} HLPFILE_LINK;

enum para_type {para_normal_text, para_debug_text, para_image};

typedef struct tagHlpFileParagraph
{
    enum para_type              cookie;

    union
    {
        struct
        {
            LPSTR                       lpszText;
            unsigned                    wFont;
            unsigned                    wIndent;
            unsigned                    wHSpace;
            unsigned                    wVSpace;
        } text;
        struct
        {
            HBITMAP                     hBitmap;
            unsigned                    pos;    /* 0: center, 1: left, 2: right */
        } image;
    } u;

    HLPFILE_LINK*               link;

    struct tagHlpFileParagraph* next;
} HLPFILE_PARAGRAPH;

typedef struct tagHlpFilePage
{
    LPSTR                       lpszTitle;
    HLPFILE_PARAGRAPH*          first_paragraph;

    unsigned                    wNumber;
    unsigned                    offset;
    struct tagHlpFilePage*      next;
    struct tagHlpFilePage*      prev;
    struct tagHlpFileFile*      file;
} HLPFILE_PAGE;

typedef struct
{
    LONG                        lHash;
    unsigned long               offset;
} HLPFILE_CONTEXT;

typedef struct tagHlpFileMacro
{
    LPCSTR                      lpszMacro;
    struct tagHlpFileMacro*     next;
} HLPFILE_MACRO;

typedef struct
{
    LOGFONT                     LogFont;
    HFONT                       hFont;
    COLORREF                    color;
} HLPFILE_FONT;

typedef struct tagHlpFileFile
{
    LPSTR                       lpszPath;
    LPSTR                       lpszTitle;
    HLPFILE_PAGE*               first_page;
    HLPFILE_MACRO*              first_macro;
    unsigned                    wContextLen;
    HLPFILE_CONTEXT*            Context;
    struct tagHlpFileFile*      prev;
    struct tagHlpFileFile*      next;

    unsigned                    wRefCount;

    unsigned short              version;
    unsigned short              flags;
    unsigned                    hasPhrases; /* Phrases or PhrIndex/PhrImage */

    unsigned                    numFonts;
    HLPFILE_FONT*               fonts;
} HLPFILE;

HLPFILE      *HLPFILE_ReadHlpFile(LPCSTR lpszPath);
HLPFILE_PAGE *HLPFILE_Contents(LPCSTR lpszPath);
HLPFILE_PAGE *HLPFILE_PageByHash(LPCSTR lpszPath, LONG wNum);
LONG          HLPFILE_Hash(LPCSTR lpszContext);
VOID          HLPFILE_FreeHlpFilePage(HLPFILE_PAGE*);
VOID          HLPFILE_FreeHlpFile(HLPFILE*);
