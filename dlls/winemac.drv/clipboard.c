/*
 * Mac clipboard driver
 *
 * Copyright 1994 Martin Ayotte
 *           1996 Alex Korobka
 *           1999 Noel Borthwick
 *           2003 Ulrich Czekalla for CodeWeavers
 * Copyright 2011, 2012, 2013 Ken Thomases for CodeWeavers Inc.
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include "macdrv.h"
#include "winuser.h"
#include "shellapi.h"
#include "shlobj.h"
#include "wine/list.h"
#include "wine/server.h"
#include "wine/unicode.h"


WINE_DEFAULT_DEBUG_CHANNEL(clipboard);


/**************************************************************************
 *              Types
 **************************************************************************/

typedef struct
{
    HWND hwnd_owner;
    UINT flags;
} CLIPBOARDINFO, *LPCLIPBOARDINFO;

typedef HANDLE (*DRVIMPORTFUNC)(CFDataRef data);
typedef CFDataRef (*DRVEXPORTFUNC)(HANDLE data);

typedef struct _WINE_CLIPFORMAT
{
    struct list             entry;
    UINT                    format_id;
    CFStringRef             type;
    DRVIMPORTFUNC           import_func;
    DRVEXPORTFUNC           export_func;
    BOOL                    synthesized;
    struct _WINE_CLIPFORMAT *natural_format;
} WINE_CLIPFORMAT;


/**************************************************************************
 *              Constants
 **************************************************************************/


/**************************************************************************
 *              Forward Function Declarations
 **************************************************************************/

static HANDLE import_clipboard_data(CFDataRef data);
static HANDLE import_bmp_to_bitmap(CFDataRef data);
static HANDLE import_bmp_to_dib(CFDataRef data);
static HANDLE import_enhmetafile(CFDataRef data);
static HANDLE import_metafilepict(CFDataRef data);
static HANDLE import_nsfilenames_to_hdrop(CFDataRef data);
static HANDLE import_utf8_to_text(CFDataRef data);
static HANDLE import_utf8_to_unicodetext(CFDataRef data);
static HANDLE import_utf16_to_unicodetext(CFDataRef data);

static CFDataRef export_clipboard_data(HANDLE data);
static CFDataRef export_bitmap_to_bmp(HANDLE data);
static CFDataRef export_dib_to_bmp(HANDLE data);
static CFDataRef export_enhmetafile(HANDLE data);
static CFDataRef export_hdrop_to_filenames(HANDLE data);
static CFDataRef export_metafilepict(HANDLE data);
static CFDataRef export_text_to_utf8(HANDLE data);
static CFDataRef export_unicodetext_to_utf8(HANDLE data);
static CFDataRef export_unicodetext_to_utf16(HANDLE data);


/**************************************************************************
 *              Static Variables
 **************************************************************************/

/* Clipboard formats */
static struct list format_list = LIST_INIT(format_list);

/*  There are two naming schemes involved and we want to have a mapping between
    them.  There are Win32 clipboard format names and there are Mac pasteboard
    types.

    The Win32 standard clipboard formats don't have names, but they are associated
    with Mac pasteboard types through the following tables, which are used to
    initialize the format_list.  Where possible, the standard clipboard formats
    are mapped to predefined pasteboard type UTIs.  Otherwise, we create Wine-
    specific types of the form "org.winehq.builtin.<format>", where <format> is
    the name of the symbolic constant for the format minus "CF_" and lowercased.
    E.g. CF_BITMAP -> org.winehq.builtin.bitmap.

    Win32 clipboard formats which originate in a Windows program may be registered
    with an arbitrary name.  We construct a Mac pasteboard type from these by
    prepending "org.winehq.registered." to the registered name.

    Likewise, Mac pasteboard types which originate in other apps may have
    arbitrary type strings.  We ignore these.

    Summary:
    Win32 clipboard format names:
        <none>                              standard clipboard format; maps via
                                            format_list to either a predefined Mac UTI
                                            or org.winehq.builtin.<format>.
        <other>                             name registered within Win32 land; maps to
                                            org.winehq.registered.<other>
    Mac pasteboard type names:
        org.winehq.builtin.<format ID>      representation of Win32 standard clipboard
                                            format for which there was no corresponding
                                            predefined Mac UTI; maps via format_list
        org.winehq.registered.<format name> representation of Win32 registered
                                            clipboard format name; maps to <format name>
        <other>                             Mac pasteboard type originating with system
                                            or other apps; either maps via format_list
                                            to a standard clipboard format or ignored
*/

static const struct
{
    UINT          id;
    CFStringRef   type;
    DRVIMPORTFUNC import;
    DRVEXPORTFUNC export;
    BOOL          synthesized;
} builtin_format_ids[] =
{
    { CF_BITMAP,            CFSTR("org.winehq.builtin.bitmap"),             import_bmp_to_bitmap,           export_bitmap_to_bmp,       FALSE },
    { CF_DIBV5,             CFSTR("org.winehq.builtin.dibv5"),              import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_DIF,               CFSTR("org.winehq.builtin.dif"),                import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_ENHMETAFILE,       CFSTR("org.winehq.builtin.enhmetafile"),        import_enhmetafile,             export_enhmetafile,         FALSE },
    { CF_LOCALE,            CFSTR("org.winehq.builtin.locale"),             import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_METAFILEPICT,      CFSTR("org.winehq.builtin.metafilepict"),       import_metafilepict,            export_metafilepict,        FALSE },
    { CF_OEMTEXT,           CFSTR("org.winehq.builtin.oemtext"),            import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_PALETTE,           CFSTR("org.winehq.builtin.palette"),            import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_PENDATA,           CFSTR("org.winehq.builtin.pendata"),            import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_RIFF,              CFSTR("org.winehq.builtin.riff"),               import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_SYLK,              CFSTR("org.winehq.builtin.sylk"),               import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_TEXT,              CFSTR("org.winehq.builtin.text"),               import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_TIFF,              CFSTR("public.tiff"),                           import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_WAVE,              CFSTR("com.microsoft.waveform-audio"),          import_clipboard_data,          export_clipboard_data,      FALSE },

    { CF_DIB,               CFSTR("org.winehq.builtin.dib"),                import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_DIB,               CFSTR("com.microsoft.bmp"),                     import_bmp_to_dib,              export_dib_to_bmp,          TRUE },

    { CF_HDROP,             CFSTR("org.winehq.builtin.hdrop"),              import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_HDROP,             CFSTR("NSFilenamesPboardType"),                 import_nsfilenames_to_hdrop,    export_hdrop_to_filenames,  TRUE },

    { CF_UNICODETEXT,       CFSTR("org.winehq.builtin.unicodetext"),        import_clipboard_data,          export_clipboard_data,      FALSE },
    { CF_UNICODETEXT,       CFSTR("public.utf16-plain-text"),               import_utf16_to_unicodetext,    export_unicodetext_to_utf16,TRUE },
    { CF_UNICODETEXT,       CFSTR("public.utf8-plain-text"),                import_utf8_to_unicodetext,     export_unicodetext_to_utf8, TRUE },
};

static const WCHAR wszRichTextFormat[] = {'R','i','c','h',' ','T','e','x','t',' ','F','o','r','m','a','t',0};
static const WCHAR wszGIF[] = {'G','I','F',0};
static const WCHAR wszJFIF[] = {'J','F','I','F',0};
static const WCHAR wszPNG[] = {'P','N','G',0};
static const WCHAR wszHTMLFormat[] = {'H','T','M','L',' ','F','o','r','m','a','t',0};
static const struct
{
    LPCWSTR       name;
    CFStringRef   type;
    DRVIMPORTFUNC import;
    DRVEXPORTFUNC export;
} builtin_format_names[] =
{
    { wszRichTextFormat,    CFSTR("public.rtf"),                            import_clipboard_data,          export_clipboard_data },
    { wszGIF,               CFSTR("com.compuserve.gif"),                    import_clipboard_data,          export_clipboard_data },
    { wszJFIF,              CFSTR("public.jpeg"),                           import_clipboard_data,          export_clipboard_data },
    { wszPNG,               CFSTR("public.png"),                            import_clipboard_data,          export_clipboard_data },
    { wszHTMLFormat,        CFSTR("public.html"),                           import_clipboard_data,          export_clipboard_data },
    { CFSTR_SHELLURLW,      CFSTR("public.url"),                            import_utf8_to_text,            export_text_to_utf8 },
};

/* The prefix prepended to a Win32 clipboard format name to make a Mac pasteboard type. */
static const CFStringRef registered_name_type_prefix = CFSTR("org.winehq.registered.");


/**************************************************************************
 *              Internal Clipboard implementation methods
 **************************************************************************/

/*
 * format_list functions
 */

/**************************************************************************
 *              debugstr_format
 */
const char *debugstr_format(UINT id)
{
    WCHAR buffer[256];

    if (GetClipboardFormatNameW(id, buffer, 256))
        return wine_dbg_sprintf("0x%04x %s", id, debugstr_w(buffer));

    switch (id)
    {
#define BUILTIN(id) case id: return #id;
    BUILTIN(CF_TEXT)
    BUILTIN(CF_BITMAP)
    BUILTIN(CF_METAFILEPICT)
    BUILTIN(CF_SYLK)
    BUILTIN(CF_DIF)
    BUILTIN(CF_TIFF)
    BUILTIN(CF_OEMTEXT)
    BUILTIN(CF_DIB)
    BUILTIN(CF_PALETTE)
    BUILTIN(CF_PENDATA)
    BUILTIN(CF_RIFF)
    BUILTIN(CF_WAVE)
    BUILTIN(CF_UNICODETEXT)
    BUILTIN(CF_ENHMETAFILE)
    BUILTIN(CF_HDROP)
    BUILTIN(CF_LOCALE)
    BUILTIN(CF_DIBV5)
    BUILTIN(CF_OWNERDISPLAY)
    BUILTIN(CF_DSPTEXT)
    BUILTIN(CF_DSPBITMAP)
    BUILTIN(CF_DSPMETAFILEPICT)
    BUILTIN(CF_DSPENHMETAFILE)
#undef BUILTIN
    default: return wine_dbg_sprintf("0x%04x", id);
    }
}


/**************************************************************************
 *              insert_clipboard_format
 */
static WINE_CLIPFORMAT *insert_clipboard_format(UINT id, CFStringRef type)
{
    WINE_CLIPFORMAT *format;

    format = HeapAlloc(GetProcessHeap(), 0, sizeof(*format));

    if (format == NULL)
    {
        WARN("No more memory for a new format!\n");
        return NULL;
    }
    format->format_id = id;
    format->import_func = import_clipboard_data;
    format->export_func = export_clipboard_data;
    format->synthesized = FALSE;
    format->natural_format = NULL;

    if (type)
        format->type = CFStringCreateCopy(NULL, type);
    else
    {
        WCHAR buffer[256];

        if (!GetClipboardFormatNameW(format->format_id, buffer, sizeof(buffer) / sizeof(buffer[0])))
        {
            WARN("failed to get name for format %s; error 0x%08x\n", debugstr_format(format->format_id), GetLastError());
            HeapFree(GetProcessHeap(), 0, format);
            return NULL;
        }

        format->type = CFStringCreateWithFormat(NULL, NULL, CFSTR("%@%S"),
                                                registered_name_type_prefix, buffer);
    }

    list_add_tail(&format_list, &format->entry);

    TRACE("Registering format %s type %s\n", debugstr_format(format->format_id),
          debugstr_cf(format->type));

    return format;
}


/**************************************************************************
 *              register_format
 *
 * Register a custom Mac clipboard format.
 */
static WINE_CLIPFORMAT* register_format(UINT id, CFStringRef type)
{
    WINE_CLIPFORMAT *format;

    /* walk format chain to see if it's already registered */
    LIST_FOR_EACH_ENTRY(format, &format_list, WINE_CLIPFORMAT, entry)
        if (format->format_id == id) return format;

    return insert_clipboard_format(id, type);
}


/**************************************************************************
 *              format_for_type
 */
static WINE_CLIPFORMAT* format_for_type(WINE_CLIPFORMAT *current, CFStringRef type)
{
    struct list *ptr = current ? &current->entry : &format_list;
    WINE_CLIPFORMAT *format = NULL;

    TRACE("current %p/%s type %s\n", current, debugstr_format(current ? current->format_id : 0), debugstr_cf(type));

    while ((ptr = list_next(&format_list, ptr)))
    {
        format = LIST_ENTRY(ptr, WINE_CLIPFORMAT, entry);
        if (CFEqual(format->type, type))
            goto done;
    }

    format = NULL;
    if (!current)
    {
        if (CFStringHasPrefix(type, CFSTR("org.winehq.builtin.")))
        {
            ERR("Shouldn't happen. Built-in type %s should have matched something in format list.\n",
                debugstr_cf(type));
        }
        else if (CFStringHasPrefix(type, registered_name_type_prefix))
        {
            LPWSTR name;
            int len = CFStringGetLength(type) - CFStringGetLength(registered_name_type_prefix);

            name = HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR));
            CFStringGetCharacters(type, CFRangeMake(CFStringGetLength(registered_name_type_prefix), len),
                                  (UniChar*)name);
            name[len] = 0;

            format = register_format(RegisterClipboardFormatW(name), type);
            if (!format)
                ERR("Failed to register format for type %s name %s\n", debugstr_cf(type), debugstr_w(name));

            HeapFree(GetProcessHeap(), 0, name);
        }
    }

done:
    TRACE(" -> %p/%s\n", format, debugstr_format(format ? format->format_id : 0));
    return format;
}


/**************************************************************************
 *              natural_format_for_format
 *
 * Find the "natural" format for this format_id (the one which isn't
 * synthesized from another type).
 */
static WINE_CLIPFORMAT* natural_format_for_format(UINT format_id)
{
    WINE_CLIPFORMAT *format;

    LIST_FOR_EACH_ENTRY(format, &format_list, WINE_CLIPFORMAT, entry)
        if (format->format_id == format_id && !format->synthesized) break;

    if (&format->entry == &format_list)
        format = NULL;

    TRACE("%s -> %p/%s\n", debugstr_format(format_id), format, debugstr_cf(format ? format->type : NULL));
    return format;
}


/***********************************************************************
 *              bitmap_info_size
 *
 * Return the size of the bitmap info structure including color table.
 */
static int bitmap_info_size(const BITMAPINFO *info, WORD coloruse)
{
    unsigned int colors, size, masks = 0;

    if (info->bmiHeader.biSize == sizeof(BITMAPCOREHEADER))
    {
        const BITMAPCOREHEADER *core = (const BITMAPCOREHEADER*)info;
        colors = (core->bcBitCount <= 8) ? 1 << core->bcBitCount : 0;
        return sizeof(BITMAPCOREHEADER) + colors *
             ((coloruse == DIB_RGB_COLORS) ? sizeof(RGBTRIPLE) : sizeof(WORD));
    }
    else  /* assume BITMAPINFOHEADER */
    {
        colors = MIN(info->bmiHeader.biClrUsed, 256);
        if (!colors && (info->bmiHeader.biBitCount <= 8))
            colors = 1 << info->bmiHeader.biBitCount;
        if (info->bmiHeader.biCompression == BI_BITFIELDS) masks = 3;
        size = max(info->bmiHeader.biSize, sizeof(BITMAPINFOHEADER) + masks * sizeof(DWORD));
        return size + colors * ((coloruse == DIB_RGB_COLORS) ? sizeof(RGBQUAD) : sizeof(WORD));
    }
}


/***********************************************************************
 *              create_dib_from_bitmap
 *
 * Allocates a packed DIB and copies the bitmap data into it.
 */
static HGLOBAL create_dib_from_bitmap(HBITMAP bitmap)
{
    HANDLE ret = 0;
    BITMAPINFOHEADER header;
    HDC hdc = GetDC(0);
    DWORD header_size;
    BITMAPINFO *bmi;

    memset(&header, 0, sizeof(header));
    header.biSize = sizeof(header);
    if (!GetDIBits(hdc, bitmap, 0, 0, NULL, (BITMAPINFO *)&header, DIB_RGB_COLORS)) goto done;

    header_size = bitmap_info_size((BITMAPINFO *)&header, DIB_RGB_COLORS);
    if (!(ret = GlobalAlloc(GMEM_FIXED, header_size + header.biSizeImage))) goto done;
    bmi = (BITMAPINFO *)ret;
    memset(bmi, 0, header_size);
    memcpy(bmi, &header, header.biSize);
    GetDIBits(hdc, bitmap, 0, abs(header.biHeight), (char *)bmi + header_size, bmi, DIB_RGB_COLORS);

done:
    ReleaseDC(0, hdc);
    return ret;
}


/**************************************************************************
 *              create_bitmap_from_dib
 *
 *  Given a packed DIB, creates a bitmap object from it.
 */
static HANDLE create_bitmap_from_dib(HANDLE dib)
{
    HANDLE ret = 0;
    BITMAPINFO *bmi;

    if (dib && (bmi = GlobalLock(dib)))
    {
        HDC hdc;
        unsigned int offset;

        hdc = GetDC(NULL);

        offset = bitmap_info_size(bmi, DIB_RGB_COLORS);

        ret = CreateDIBitmap(hdc, &bmi->bmiHeader, CBM_INIT, (LPBYTE)bmi + offset,
                             bmi, DIB_RGB_COLORS);

        GlobalUnlock(dib);
        ReleaseDC(NULL, hdc);
    }

    return ret;
}


/**************************************************************************
 *              import_clipboard_data
 *
 *  Generic import clipboard data routine.
 */
static HANDLE import_clipboard_data(CFDataRef data)
{
    HANDLE data_handle = NULL;

    size_t len = CFDataGetLength(data);
    if (len)
    {
        LPVOID p;

        /* Turn on the DDESHARE flag to enable shared 32 bit memory */
        data_handle = GlobalAlloc(GMEM_FIXED, len);
        if (!data_handle)
            return NULL;

        if ((p = GlobalLock(data_handle)))
        {
            memcpy(p, CFDataGetBytePtr(data), len);
            GlobalUnlock(data_handle);
        }
        else
        {
            GlobalFree(data_handle);
            data_handle = NULL;
        }
    }

    return data_handle;
}


/**************************************************************************
 *              import_bmp_to_bitmap
 *
 *  Import BMP data, converting to CF_BITMAP format.
 */
static HANDLE import_bmp_to_bitmap(CFDataRef data)
{
    HANDLE ret;
    HANDLE dib = import_bmp_to_dib(data);

    ret = create_bitmap_from_dib(dib);

    GlobalFree(dib);
    return ret;
}


/**************************************************************************
 *              import_bmp_to_dib
 *
 *  Import BMP data, converting to CF_DIB or CF_DIBV5 format.  This just
 *  entails stripping the BMP file format header.
 */
static HANDLE import_bmp_to_dib(CFDataRef data)
{
    HANDLE ret = 0;
    BITMAPFILEHEADER *bfh = (BITMAPFILEHEADER*)CFDataGetBytePtr(data);
    CFIndex len = CFDataGetLength(data);

    if (len >= sizeof(*bfh) + sizeof(BITMAPCOREHEADER) &&
        bfh->bfType == 0x4d42 /* "BM" */)
    {
        BITMAPINFO *bmi = (BITMAPINFO*)(bfh + 1);
        BYTE* p;

        len -= sizeof(*bfh);
        ret = GlobalAlloc(GMEM_FIXED, len);
        if (!ret || !(p = GlobalLock(ret)))
        {
            GlobalFree(ret);
            return 0;
        }

        memcpy(p, bmi, len);
        GlobalUnlock(ret);
    }

    return ret;
}


/**************************************************************************
 *              import_enhmetafile
 *
 *  Import enhanced metafile data, converting it to CF_ENHMETAFILE.
 */
static HANDLE import_enhmetafile(CFDataRef data)
{
    HANDLE ret = 0;
    CFIndex len = CFDataGetLength(data);

    TRACE("data %s\n", debugstr_cf(data));

    if (len)
        ret = SetEnhMetaFileBits(len, (const BYTE*)CFDataGetBytePtr(data));

    return ret;
}


/**************************************************************************
 *              import_metafilepict
 *
 *  Import metafile picture data, converting it to CF_METAFILEPICT.
 */
static HANDLE import_metafilepict(CFDataRef data)
{
    HANDLE ret = 0;
    CFIndex len = CFDataGetLength(data);
    METAFILEPICT *mfp;

    TRACE("data %s\n", debugstr_cf(data));

    if (len >= sizeof(*mfp) && (ret = GlobalAlloc(GMEM_FIXED, sizeof(*mfp))))
    {
        const BYTE *bytes = (const BYTE*)CFDataGetBytePtr(data);

        mfp = GlobalLock(ret);
        memcpy(mfp, bytes, sizeof(*mfp));
        mfp->hMF = SetMetaFileBitsEx(len - sizeof(*mfp), bytes + sizeof(*mfp));
        GlobalUnlock(ret);
    }

    return ret;
}


/**************************************************************************
 *              import_nsfilenames_to_hdrop
 *
 *  Import NSFilenamesPboardType data, converting the property-list-
 *  serialized array of path strings to CF_HDROP.
 */
static HANDLE import_nsfilenames_to_hdrop(CFDataRef data)
{
    HDROP hdrop = NULL;
    CFArrayRef names;
    CFIndex count, i;
    size_t len;
    char *buffer = NULL;
    WCHAR **paths = NULL;
    DROPFILES* dropfiles;
    UniChar* p;

    TRACE("data %s\n", debugstr_cf(data));

    names = (CFArrayRef)CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable,
                                                     NULL, NULL);
    if (!names || CFGetTypeID(names) != CFArrayGetTypeID())
    {
        WARN("failed to interpret data as a CFArray\n");
        goto done;
    }

    count = CFArrayGetCount(names);

    len = 0;
    for (i = 0; i < count; i++)
    {
        CFIndex this_len;
        CFStringRef name = (CFStringRef)CFArrayGetValueAtIndex(names, i);
        TRACE("    %s\n", debugstr_cf(name));
        if (CFGetTypeID(name) != CFStringGetTypeID())
        {
            WARN("non-string in array\n");
            goto done;
        }

        this_len = CFStringGetMaximumSizeOfFileSystemRepresentation(name);
        if (this_len > len)
            len = this_len;
    }

    buffer = HeapAlloc(GetProcessHeap(), 0, len);
    if (!buffer)
    {
        WARN("failed to allocate buffer for file-system representations\n");
        goto done;
    }

    paths = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(paths[0]));
    if (!paths)
    {
        WARN("failed to allocate array of DOS paths\n");
        goto done;
    }

    for (i = 0; i < count; i++)
    {
        CFStringRef name = (CFStringRef)CFArrayGetValueAtIndex(names, i);
        if (!CFStringGetFileSystemRepresentation(name, buffer, len))
        {
            WARN("failed to get file-system representation for %s\n", debugstr_cf(name));
            goto done;
        }
        paths[i] = wine_get_dos_file_name(buffer);
        if (!paths[i])
        {
            WARN("failed to get DOS path for %s\n", debugstr_a(buffer));
            goto done;
        }
    }

    len = 1; /* for the terminating null */
    for (i = 0; i < count; i++)
        len += strlenW(paths[i]) + 1;

    hdrop = GlobalAlloc(GMEM_FIXED, sizeof(*dropfiles) + len * sizeof(WCHAR));
    if (!hdrop || !(dropfiles = GlobalLock(hdrop)))
    {
        WARN("failed to allocate HDROP\n");
        GlobalFree(hdrop);
        hdrop = NULL;
        goto done;
    }

    dropfiles->pFiles   = sizeof(*dropfiles);
    dropfiles->pt.x     = 0;
    dropfiles->pt.y     = 0;
    dropfiles->fNC      = FALSE;
    dropfiles->fWide    = TRUE;

    p = (WCHAR*)(dropfiles + 1);
    for (i = 0; i < count; i++)
    {
        strcpyW(p, paths[i]);
        p += strlenW(p) + 1;
    }
    *p = 0;

    GlobalUnlock(hdrop);

done:
    if (paths)
    {
        for (i = 0; i < count; i++)
            HeapFree(GetProcessHeap(), 0, paths[i]);
        HeapFree(GetProcessHeap(), 0, paths);
    }
    HeapFree(GetProcessHeap(), 0, buffer);
    if (names) CFRelease(names);
    return hdrop;
}


/**************************************************************************
 *              import_utf8_to_text
 *
 *  Import a UTF-8 string, converting the string to CF_TEXT.
 */
static HANDLE import_utf8_to_text(CFDataRef data)
{
    HANDLE ret = NULL;
    HANDLE unicode_handle = import_utf8_to_unicodetext(data);
    LPWSTR unicode_string = GlobalLock(unicode_handle);

    if (unicode_string)
    {
        int unicode_len;
        HANDLE handle;
        char *p;
        INT len;

        unicode_len = GlobalSize(unicode_handle) / sizeof(WCHAR);

        len = WideCharToMultiByte(CP_ACP, 0, unicode_string, unicode_len, NULL, 0, NULL, NULL);
        if (!unicode_len || unicode_string[unicode_len - 1]) len += 1;
        handle = GlobalAlloc(GMEM_FIXED, len);

        if (handle && (p = GlobalLock(handle)))
        {
            WideCharToMultiByte(CP_ACP, 0, unicode_string, unicode_len, p, len, NULL, NULL);
            p[len - 1] = 0;
            GlobalUnlock(handle);
            ret = handle;
        }
        GlobalUnlock(unicode_handle);
    }

    GlobalFree(unicode_handle);
    return ret;
}


/**************************************************************************
 *              import_utf8_to_unicodetext
 *
 *  Import a UTF-8 string, converting the string to CF_UNICODETEXT.
 */
static HANDLE import_utf8_to_unicodetext(CFDataRef data)
{
    const BYTE *src;
    unsigned long src_len;
    unsigned long new_lines = 0;
    LPSTR dst;
    unsigned long i, j;
    HANDLE unicode_handle = NULL;

    src = CFDataGetBytePtr(data);
    src_len = CFDataGetLength(data);
    for (i = 0; i < src_len; i++)
    {
        if (src[i] == '\n')
            new_lines++;
    }

    if ((dst = HeapAlloc(GetProcessHeap(), 0, src_len + new_lines + 1)))
    {
        UINT count;

        for (i = 0, j = 0; i < src_len; i++)
        {
            if (src[i] == '\n')
                dst[j++] = '\r';

            dst[j++] = src[i];
        }
        dst[j] = 0;

        count = MultiByteToWideChar(CP_UTF8, 0, dst, -1, NULL, 0);
        unicode_handle = GlobalAlloc(GMEM_FIXED, count * sizeof(WCHAR));

        if (unicode_handle)
        {
            WCHAR *textW = GlobalLock(unicode_handle);
            MultiByteToWideChar(CP_UTF8, 0, dst, -1, textW, count);
            GlobalUnlock(unicode_handle);
        }

        HeapFree(GetProcessHeap(), 0, dst);
    }

    return unicode_handle;
}


/**************************************************************************
 *              import_utf16_to_unicodetext
 *
 *  Import a UTF-8 string, converting the string to CF_UNICODETEXT.
 */
static HANDLE import_utf16_to_unicodetext(CFDataRef data)
{
    const WCHAR *src;
    unsigned long src_len;
    unsigned long new_lines = 0;
    LPWSTR dst;
    unsigned long i, j;
    HANDLE unicode_handle;

    src = (const WCHAR *)CFDataGetBytePtr(data);
    src_len = CFDataGetLength(data) / sizeof(WCHAR);
    for (i = 0; i < src_len; i++)
    {
        if (src[i] == '\n')
            new_lines++;
        else if (src[i] == '\r' && (i + 1 >= src_len || src[i + 1] != '\n'))
            new_lines++;
    }

    if ((unicode_handle = GlobalAlloc(GMEM_FIXED, (src_len + new_lines + 1) * sizeof(WCHAR))))
    {
        dst = GlobalLock(unicode_handle);

        for (i = 0, j = 0; i < src_len; i++)
        {
            if (src[i] == '\n')
                dst[j++] = '\r';

            dst[j++] = src[i];

            if (src[i] == '\r' && (i + 1 >= src_len || src[i + 1] != '\n'))
                dst[j++] = '\n';
        }
        dst[j] = 0;

        GlobalUnlock(unicode_handle);
    }

    return unicode_handle;
}


/**************************************************************************
 *              export_clipboard_data
 *
 *  Generic export clipboard data routine.
 */
static CFDataRef export_clipboard_data(HANDLE data)
{
    CFDataRef ret;
    UINT len;
    LPVOID src;

    len = GlobalSize(data);
    src = GlobalLock(data);
    if (!src) return NULL;

    ret = CFDataCreate(NULL, src, len);
    GlobalUnlock(data);

    return ret;
}


/**************************************************************************
 *              export_bitmap_to_bmp
 *
 *  Export CF_BITMAP to BMP file format.
 */
static CFDataRef export_bitmap_to_bmp(HANDLE data)
{
    CFDataRef ret = NULL;
    HGLOBAL dib;

    dib = create_dib_from_bitmap(data);
    if (dib)
    {
        ret = export_dib_to_bmp(dib);
        GlobalFree(dib);
    }

    return ret;
}


/**************************************************************************
 *              export_dib_to_bmp
 *
 *  Export CF_DIB or CF_DIBV5 to BMP file format.  This just entails
 *  prepending a BMP file format header to the data.
 */
static CFDataRef export_dib_to_bmp(HANDLE data)
{
    CFMutableDataRef ret = NULL;
    BYTE *dibdata;
    CFIndex len;
    BITMAPFILEHEADER bfh;

    dibdata = GlobalLock(data);
    if (!dibdata)
        return NULL;

    len = sizeof(bfh) + GlobalSize(data);
    ret = CFDataCreateMutable(NULL, len);
    if (ret)
    {
        bfh.bfType = 0x4d42; /* "BM" */
        bfh.bfSize = len;
        bfh.bfReserved1 = 0;
        bfh.bfReserved2 = 0;
        bfh.bfOffBits = sizeof(bfh) + bitmap_info_size((BITMAPINFO*)dibdata, DIB_RGB_COLORS);
        CFDataAppendBytes(ret, (UInt8*)&bfh, sizeof(bfh));

        /* rest of bitmap is the same as the packed dib */
        CFDataAppendBytes(ret, (UInt8*)dibdata, len - sizeof(bfh));
    }

    GlobalUnlock(data);

    return ret;
}


/**************************************************************************
 *              export_enhmetafile
 *
 *  Export an enhanced metafile to data.
 */
static CFDataRef export_enhmetafile(HANDLE data)
{
    CFMutableDataRef ret = NULL;
    unsigned int size = GetEnhMetaFileBits(data, 0, NULL);

    TRACE("data %p\n", data);

    ret = CFDataCreateMutable(NULL, size);
    if (ret)
    {
        CFDataSetLength(ret, size);
        GetEnhMetaFileBits(data, size, (BYTE*)CFDataGetMutableBytePtr(ret));
    }

    TRACE(" -> %s\n", debugstr_cf(ret));
    return ret;
}


/**************************************************************************
 *              export_hdrop_to_filenames
 *
 *  Export CF_HDROP to NSFilenamesPboardType data, which is a CFArray of
 *  CFStrings (holding Unix paths) which is serialized as a property list.
 */
static CFDataRef export_hdrop_to_filenames(HANDLE data)
{
    CFDataRef ret = NULL;
    DROPFILES *dropfiles;
    CFMutableArrayRef filenames = NULL;
    void *p;
    WCHAR *buffer = NULL;
    size_t buffer_len = 0;

    TRACE("data %p\n", data);

    if (!(dropfiles = GlobalLock(data)))
    {
        WARN("failed to lock data %p\n", data);
        goto done;
    }

    filenames = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (!filenames)
    {
        WARN("failed to create filenames array\n");
        goto done;
    }

    p = (char*)dropfiles + dropfiles->pFiles;
    while (dropfiles->fWide ? *(WCHAR*)p : *(char*)p)
    {
        char *unixname;
        CFStringRef filename;

        TRACE("    %s\n", dropfiles->fWide ? debugstr_w(p) : debugstr_a(p));

        if (dropfiles->fWide)
            unixname = wine_get_unix_file_name(p);
        else
        {
            int len = MultiByteToWideChar(CP_ACP, 0, p, -1, NULL, 0);
            if (len)
            {
                if (len > buffer_len)
                {
                    HeapFree(GetProcessHeap(), 0, buffer);
                    buffer_len = len * 2;
                    buffer = HeapAlloc(GetProcessHeap(), 0, buffer_len * sizeof(*buffer));
                }

                MultiByteToWideChar(CP_ACP, 0, p, -1, buffer, buffer_len);
                unixname = wine_get_unix_file_name(buffer);
            }
            else
                unixname = NULL;
        }
        if (!unixname)
        {
            WARN("failed to convert DOS path to Unix: %s\n",
                 dropfiles->fWide ? debugstr_w(p) : debugstr_a(p));
            goto done;
        }

        if (dropfiles->fWide)
            p = (WCHAR*)p + strlenW(p) + 1;
        else
            p = (char*)p + strlen(p) + 1;

        filename = CFStringCreateWithFileSystemRepresentation(NULL, unixname);
        HeapFree(GetProcessHeap(), 0, unixname);
        if (!filename)
        {
            WARN("failed to create CFString from Unix path %s\n", debugstr_a(unixname));
            goto done;
        }

        CFArrayAppendValue(filenames, filename);
        CFRelease(filename);
    }

    ret = CFPropertyListCreateData(NULL, filenames, kCFPropertyListXMLFormat_v1_0, 0, NULL);

done:
    HeapFree(GetProcessHeap(), 0, buffer);
    GlobalUnlock(data);
    if (filenames) CFRelease(filenames);
    TRACE(" -> %s\n", debugstr_cf(ret));
    return ret;
}


/**************************************************************************
 *              export_metafilepict
 *
 *  Export a metafile to data.
 */
static CFDataRef export_metafilepict(HANDLE data)
{
    CFMutableDataRef ret = NULL;
    METAFILEPICT *mfp = GlobalLock(data);
    unsigned int size = GetMetaFileBitsEx(mfp->hMF, 0, NULL);

    TRACE("data %p\n", data);

    ret = CFDataCreateMutable(NULL, sizeof(*mfp) + size);
    if (ret)
    {
        CFDataAppendBytes(ret, (UInt8*)mfp, sizeof(*mfp));
        CFDataIncreaseLength(ret, size);
        GetMetaFileBitsEx(mfp->hMF, size, (BYTE*)CFDataGetMutableBytePtr(ret) + sizeof(*mfp));
    }

    GlobalUnlock(data);
    TRACE(" -> %s\n", debugstr_cf(ret));
    return ret;
}


/**************************************************************************
 *              export_text_to_utf8
 *
 *  Export CF_TEXT to UTF-8.
 */
static CFDataRef export_text_to_utf8(HANDLE data)
{
    CFDataRef ret = NULL;
    const char* str;

    if ((str = GlobalLock(data)))
    {
        int str_len = GlobalSize(data);
        int wstr_len;
        WCHAR *wstr;
        HANDLE unicode;
        char *p;

        wstr_len = MultiByteToWideChar(CP_ACP, 0, str, str_len, NULL, 0);
        if (!str_len || str[str_len - 1]) wstr_len += 1;
        wstr = HeapAlloc(GetProcessHeap(), 0, wstr_len * sizeof(WCHAR));
        MultiByteToWideChar(CP_ACP, 0, str, str_len, wstr, wstr_len);
        wstr[wstr_len - 1] = 0;

        unicode = GlobalAlloc(GMEM_FIXED, wstr_len * sizeof(WCHAR));
        if (unicode && (p = GlobalLock(unicode)))
        {
            memcpy(p, wstr, wstr_len * sizeof(WCHAR));
            GlobalUnlock(unicode);
        }

        ret = export_unicodetext_to_utf8(unicode);

        GlobalFree(unicode);
        GlobalUnlock(data);
    }

    return ret;
}


/**************************************************************************
 *              export_unicodetext_to_utf8
 *
 *  Export CF_UNICODETEXT to UTF-8.
 */
static CFDataRef export_unicodetext_to_utf8(HANDLE data)
{
    CFMutableDataRef ret;
    LPVOID src;
    INT dst_len;

    src = GlobalLock(data);
    if (!src) return NULL;

    dst_len = WideCharToMultiByte(CP_UTF8, 0, src, -1, NULL, 0, NULL, NULL);
    if (dst_len) dst_len--; /* Leave off null terminator. */
    ret = CFDataCreateMutable(NULL, dst_len);
    if (ret)
    {
        LPSTR dst;
        int i, j;

        CFDataSetLength(ret, dst_len);
        dst = (LPSTR)CFDataGetMutableBytePtr(ret);
        WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_len, NULL, NULL);

        /* Remove carriage returns */
        for (i = 0, j = 0; i < dst_len; i++)
        {
            if (dst[i] == '\r' &&
                (i + 1 >= dst_len || dst[i + 1] == '\n' || dst[i + 1] == '\0'))
                continue;
            dst[j++] = dst[i];
        }
        CFDataSetLength(ret, j);
    }
    GlobalUnlock(data);

    return ret;
}


/**************************************************************************
 *              export_unicodetext_to_utf16
 *
 *  Export CF_UNICODETEXT to UTF-16.
 */
static CFDataRef export_unicodetext_to_utf16(HANDLE data)
{
    CFMutableDataRef ret;
    const WCHAR *src;
    INT src_len;

    src = GlobalLock(data);
    if (!src) return NULL;

    src_len = GlobalSize(data) / sizeof(WCHAR);
    if (src_len) src_len--; /* Leave off null terminator. */
    ret = CFDataCreateMutable(NULL, src_len * sizeof(WCHAR));
    if (ret)
    {
        LPWSTR dst;
        int i, j;

        CFDataSetLength(ret, src_len * sizeof(WCHAR));
        dst = (LPWSTR)CFDataGetMutableBytePtr(ret);

        /* Remove carriage returns */
        for (i = 0, j = 0; i < src_len; i++)
        {
            if (src[i] == '\r' &&
                (i + 1 >= src_len || src[i + 1] == '\n' || src[i + 1] == '\0'))
                continue;
            dst[j++] = src[i];
        }
        CFDataSetLength(ret, j * sizeof(WCHAR));
    }
    GlobalUnlock(data);

    return ret;
}


/**************************************************************************
 *              get_clipboard_info
 */
static BOOL get_clipboard_info(LPCLIPBOARDINFO cbinfo)
{
    BOOL ret = FALSE;

    SERVER_START_REQ(set_clipboard_info)
    {
        req->flags = 0;

        if (wine_server_call_err(req))
        {
            ERR("Failed to get clipboard owner.\n");
        }
        else
        {
            cbinfo->hwnd_owner = wine_server_ptr_handle(reply->old_owner);
            cbinfo->flags = reply->flags;

            ret = TRUE;
        }
    }
    SERVER_END_REQ;

    return ret;
}


/**************************************************************************
 *              macdrv_get_pasteboard_data
 */
HANDLE macdrv_get_pasteboard_data(CFTypeRef pasteboard, UINT desired_format)
{
    CFArrayRef types;
    CFIndex count;
    CFIndex i;
    CFStringRef type, best_type;
    WINE_CLIPFORMAT* best_format = NULL;
    HANDLE data = NULL;

    TRACE("pasteboard %p, desired_format %s\n", pasteboard, debugstr_format(desired_format));

    types = macdrv_copy_pasteboard_types(pasteboard);
    if (!types)
    {
        WARN("Failed to copy pasteboard types\n");
        return NULL;
    }

    count = CFArrayGetCount(types);
    TRACE("got %ld types\n", count);

    for (i = 0; (!best_format || best_format->synthesized) && i < count; i++)
    {
        WINE_CLIPFORMAT* format;

        type = CFArrayGetValueAtIndex(types, i);

        format = NULL;
        while ((!best_format || best_format->synthesized) && (format = format_for_type(format, type)))
        {
            TRACE("for type %s got format %p/%s\n", debugstr_cf(type), format, debugstr_format(format ? format->format_id : 0));

            if (format->format_id == desired_format)
            {
                /* The best format is the matching one which is not synthesized.  Failing that,
                   the best format is the first matching synthesized format. */
                if (!format->synthesized || !best_format)
                {
                    best_type = type;
                    best_format = format;
                }
            }
        }
    }

    if (best_format)
    {
        CFDataRef pasteboard_data = macdrv_copy_pasteboard_data(pasteboard, best_type);

        TRACE("got pasteboard data for type %s: %s\n", debugstr_cf(best_type), debugstr_cf(pasteboard_data));

        if (pasteboard_data)
        {
            data = best_format->import_func(pasteboard_data);
            CFRelease(pasteboard_data);
        }
    }

    CFRelease(types);
    TRACE(" -> %p\n", data);
    return data;
}


/**************************************************************************
 *              macdrv_pasteboard_has_format
 */
BOOL macdrv_pasteboard_has_format(CFTypeRef pasteboard, UINT desired_format)
{
    CFArrayRef types;
    int count;
    UINT i;
    BOOL found = FALSE;

    TRACE("pasteboard %p, desired_format %s\n", pasteboard, debugstr_format(desired_format));

    types = macdrv_copy_pasteboard_types(pasteboard);
    if (!types)
    {
        WARN("Failed to copy pasteboard types\n");
        return FALSE;
    }

    count = CFArrayGetCount(types);
    TRACE("got %d types\n", count);

    for (i = 0; !found && i < count; i++)
    {
        CFStringRef type = CFArrayGetValueAtIndex(types, i);
        WINE_CLIPFORMAT* format;

        format = NULL;
        while (!found && (format = format_for_type(format, type)))
        {
            TRACE("for type %s got format %s\n", debugstr_cf(type), debugstr_format(format->format_id));

            if (format->format_id == desired_format)
                found = TRUE;
        }
    }

    CFRelease(types);
    TRACE(" -> %d\n", found);
    return found;
}


/**************************************************************************
 *              macdrv_copy_pasteboard_formats
 */
CFArrayRef macdrv_copy_pasteboard_formats(CFTypeRef pasteboard)
{
    CFArrayRef types;
    CFIndex count;
    CFMutableArrayRef formats;
    CFIndex i;
    WINE_CLIPFORMAT* format;

    TRACE("pasteboard %p\n", pasteboard);

    types = macdrv_copy_pasteboard_types(pasteboard);
    if (!types)
    {
        WARN("Failed to copy pasteboard types\n");
        return NULL;
    }

    count = CFArrayGetCount(types);
    TRACE("got %ld types\n", count);

    if (!count)
    {
        CFRelease(types);
        return NULL;
    }

    formats = CFArrayCreateMutable(NULL, 0, NULL);
    if (!formats)
    {
        WARN("Failed to allocate formats array\n");
        CFRelease(types);
        return NULL;
    }

    for (i = 0; i < count; i++)
    {
        CFStringRef type = CFArrayGetValueAtIndex(types, i);
        BOOL found = FALSE;

        format = NULL;
        while ((format = format_for_type(format, type)))
        {
            /* Suppose type is "public.utf8-plain-text".  format->format_id will be each of
               CF_TEXT, CF_OEMTEXT, and CF_UNICODETEXT in turn.  We want to look up the natural
               type for each of those IDs (e.g. CF_TEXT -> "org.winehq.builtin.text") and then see
               if that type is present in the pasteboard.  If it is, then we don't want to add the
               format to the list yet because it would be out of order.

               For example, if a Mac app put "public.utf8-plain-text" and "public.tiff" on the
               pasteboard, then we want the Win32 clipboard formats to be CF_TEXT, CF_OEMTEXT, and
               CF_UNICODETEXT, and CF_TIFF, in that order.  All of the text formats belong before
               CF_TIFF because the Mac app expressed that text was "better" than the TIFF.  In
               this case, as soon as we encounter "public.utf8-plain-text" we should add all of
               the associated text format IDs.

               But if a Wine process put "org.winehq.builtin.unicodetext",
               "public.utf8-plain-text", "public.utf16-plain-text", and "public.tiff", then we
               want the clipboard formats to be CF_UNICODETEXT, CF_TIFF, CF_TEXT, and CF_OEMTEXT,
               in that order.  The Windows program presumably added CF_UNICODETEXT and CF_TIFF.
               We're synthesizing CF_TEXT and CF_OEMTEXT from CF_UNICODETEXT but we want them to
               come after the non-synthesized CF_TIFF.  In this case, we don't want to add the
               text formats upon encountering "public.utf8-plain-text",

               We tell the two cases apart by seeing that one of the natural types for the text
               formats (i.e. "org.winehq.builtin.unicodetext") is present on the pasteboard.
               "found" indicates that. */

            if (!format->synthesized)
            {
                TRACE("for type %s got primary format %p/%s\n", debugstr_cf(type), format, debugstr_format(format->format_id));
                CFArrayAppendValue(formats, (void*)format->format_id);
                found = TRUE;
            }
            else if (!found && format->natural_format &&
                     CFArrayContainsValue(types, CFRangeMake(0, count), format->natural_format->type))
            {
                TRACE("for type %s deferring synthesized formats because type %s is also present\n",
                      debugstr_cf(type), debugstr_cf(format->natural_format->type));
                found = TRUE;
            }
        }

        if (!found)
        {
            while ((format = format_for_type(format, type)))
            {
                /* Don't override a real value with a synthesized value. */
                if (!CFArrayContainsValue(formats, CFRangeMake(0, CFArrayGetCount(formats)), (void*)format->format_id))
                {
                    TRACE("for type %s got synthesized format %p/%s\n", debugstr_cf(type), format, debugstr_format(format->format_id));
                    CFArrayAppendValue(formats, (void*)format->format_id);
                }
            }
        }
    }

    /* Now go back through the types adding the synthesized formats that we deferred before. */
    for (i = 0; i < count; i++)
    {
        CFStringRef type = CFArrayGetValueAtIndex(types, i);

        format = NULL;
        while ((format = format_for_type(format, type)))
        {
            if (format->synthesized)
            {
                /* Don't override a real value with a synthesized value. */
                if (!CFArrayContainsValue(formats, CFRangeMake(0, CFArrayGetCount(formats)), (void*)format->format_id))
                {
                    TRACE("for type %s got synthesized format %p/%s\n", debugstr_cf(type), format, debugstr_format(format->format_id));
                    CFArrayAppendValue(formats, (void*)format->format_id);
                }
            }
        }
    }

    CFRelease(types);

    TRACE(" -> %s\n", debugstr_cf(formats));
    return formats;
}


/**************************************************************************
 *              Mac User Driver Clipboard Exports
 **************************************************************************/


/**************************************************************************
 *              MACDRV Private Clipboard Exports
 **************************************************************************/


/**************************************************************************
 *              macdrv_clipboard_process_attach
 */
void macdrv_clipboard_process_attach(void)
{
    UINT i;
    WINE_CLIPFORMAT *format;

    /* Register built-in formats */
    for (i = 0; i < sizeof(builtin_format_ids)/sizeof(builtin_format_ids[0]); i++)
    {
        if (!(format = HeapAlloc(GetProcessHeap(), 0, sizeof(*format)))) break;
        format->format_id       = builtin_format_ids[i].id;
        format->type            = CFRetain(builtin_format_ids[i].type);
        format->import_func     = builtin_format_ids[i].import;
        format->export_func     = builtin_format_ids[i].export;
        format->synthesized     = builtin_format_ids[i].synthesized;
        format->natural_format  = NULL;
        list_add_tail(&format_list, &format->entry);
    }

    LIST_FOR_EACH_ENTRY(format, &format_list, WINE_CLIPFORMAT, entry)
    {
        if (format->synthesized)
            format->natural_format = natural_format_for_format(format->format_id);
    }

    /* Register known mappings between Windows formats and Mac types */
    for (i = 0; i < sizeof(builtin_format_names)/sizeof(builtin_format_names[0]); i++)
    {
        if (!(format = HeapAlloc(GetProcessHeap(), 0, sizeof(*format)))) break;
        format->format_id       = RegisterClipboardFormatW(builtin_format_names[i].name);
        format->type            = CFRetain(builtin_format_names[i].type);
        format->import_func     = builtin_format_names[i].import;
        format->export_func     = builtin_format_names[i].export;
        format->synthesized     = FALSE;
        format->natural_format  = NULL;
        list_add_tail(&format_list, &format->entry);
    }
}


/**************************************************************************
 *              query_pasteboard_data
 */
BOOL query_pasteboard_data(HWND hwnd, CFStringRef type)
{
    BOOL ret = FALSE;
    CLIPBOARDINFO cbinfo;
    WINE_CLIPFORMAT* format;
    CFArrayRef types = NULL;
    CFRange range;

    TRACE("hwnd %p type %s\n", hwnd, debugstr_cf(type));

    if (get_clipboard_info(&cbinfo))
        hwnd = cbinfo.hwnd_owner;

    format = NULL;
    while ((format = format_for_type(format, type)))
    {
        TRACE("for type %s got format %p/%s\n", debugstr_cf(type), format, debugstr_format(format->format_id));

        if (!format->synthesized)
        {
            TRACE("Sending WM_RENDERFORMAT message for format %s to hwnd %p\n", debugstr_format(format->format_id), hwnd);
            SendMessageW(hwnd, WM_RENDERFORMAT, format->format_id, 0);
            ret = TRUE;
            goto done;
        }

        if (!types)
        {
            types = macdrv_copy_pasteboard_types(NULL);
            if (!types)
            {
                WARN("Failed to copy pasteboard types\n");
                break;
            }

            range = CFRangeMake(0, CFArrayGetCount(types));
        }

        /* The type maps to a synthesized format.  Now look up what type that format maps to natively
           (not synthesized).  For example, if type is "public.utf8-plain-text", then this format may
           have an ID of CF_TEXT.  From CF_TEXT, we want to find "org.winehq.builtin.text" to see if
           that type is present in the pasteboard.  If it is, then the app must have promised it and
           we can ask it to render it.  (If it had put it on the clipboard immediately, then the
           pasteboard would also have data for "public.utf8-plain-text" and we wouldn't be here.)  If
           "org.winehq.builtin.text" is not on the pasteboard, then one of the other text formats is
           presumably responsible for the promise that we're trying to satisfy, so we keep looking. */
        if (format->natural_format && CFArrayContainsValue(types, range, format->natural_format->type))
        {
            TRACE("Sending WM_RENDERFORMAT message for format %s to hwnd %p\n", debugstr_format(format->format_id), hwnd);
            SendMessageW(hwnd, WM_RENDERFORMAT, format->format_id, 0);
            ret = TRUE;
            goto done;
        }
    }

done:
    if (types) CFRelease(types);

    return ret;
}
