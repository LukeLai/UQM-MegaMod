//Copyright Paul Reiche, Fred Ford. 1992-2002

/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <string.h>
#include <stdio.h>

#include "options.h"
#include "port.h"
#include "uqm/units.h"
#include "libs/uio.h"
#include "libs/reslib.h"
		// for _cur_resfile_name
#include "libs/log.h"
#include "libs/memlib.h"
#include "libs/graphics/tfb_draw.h"
#include "libs/graphics/drawable.h"
#include "libs/graphics/font.h"

typedef struct anidata
{
	int transparent_color;
	int colormap_index;
	int hotspot_x;
	int hotspot_y;
} AniData;

extern uio_Repository *repository;
static uio_AutoMount *autoMount[] = { NULL };

static void
process_image (FRAME FramePtr, TFB_Canvas img[], AniData *ani, int cel_ct)
{
	TFB_Image *tfbimg;
	int hx, hy;

	FramePtr->Type = ROM_DRAWABLE;
	FramePtr->Index = cel_ct;

	// handle transparency cases
	if (TFB_DrawCanvas_IsPaletted (img[cel_ct]))
	{	// indexed color image
		if (ani[cel_ct].transparent_color >= 0)
		{
		    TFB_DrawCanvas_SetTransparentIndex (img[cel_ct],
					ani[cel_ct].transparent_color, FALSE);
		}
	}
	else
	{	// special transparency cases for truecolor images
		if (ani[cel_ct].transparent_color == 0)
		{	// make RGB=0,0,0 transparent
			Color color = {0, 0, 0, 0};
		    TFB_DrawCanvas_SetTransparentColor (img[cel_ct], color, FALSE);
		}
	}
	if (ani[cel_ct].transparent_color == -1)
	{	// enforce -1 to mean 'no transparency'
		TFB_DrawCanvas_SetTransparentIndex (img[cel_ct], -1, FALSE);
		// set transparent_color == -2 to use PNG tRNS transparency
	}
	
	hx = ani[cel_ct].hotspot_x;
	hy = ani[cel_ct].hotspot_y;

	FramePtr->image = TFB_DrawImage_New (img[cel_ct]);

	tfbimg = FramePtr->image;
	tfbimg->colormap_index = ani[cel_ct].colormap_index;
	img[cel_ct] = tfbimg->NormalImg;
	
	FramePtr->HotSpot = MAKE_HOT_SPOT (hx, hy);
	SetFrameBounds (FramePtr, tfbimg->extent.width, tfbimg->extent.height);

#ifdef CLIPDEBUG
	{
		/* for debugging clipping:
		   draws white (or most matching color from palette) pixels to
	       every corner of the image
		 */
		Color color = {0xff, 0xff, 0xff, 0xff};
		RECT r = {{0, 0}, {1, 1}};
		if (tfbimg->extent.width > 2 && tfbimg->extent.height > 2)
		{
			TFB_DrawImage_Rect (&r, color, tfbimg);
			r.corner.x = tfbimg->extent.width - 1;
			TFB_DrawImage_Rect (&r, color, tfbimg);
			r.corner.y = tfbimg->extent.height - 1;
			TFB_DrawImage_Rect (&r, color, tfbimg);
			r.corner.x = 0;
			TFB_DrawImage_Rect (&r, color, tfbimg);
		}
	}
#endif
}

static void
processFontChar (TFB_Char* CharPtr, TFB_Canvas canvas, FONT fontPtr)
{
	BYTE* newdata;
	size_t dpitch;

	TFB_DrawCanvas_GetExtent (canvas, &CharPtr->extent);

	// Currently, each font char has its own separate data
	// but that can change to common mem area
	dpitch = CharPtr->extent.width;
	newdata = HMalloc (dpitch * CharPtr->extent.height * sizeof (BYTE));
	TFB_DrawCanvas_GetFontCharData (canvas, newdata, dpitch);

	CharPtr->data = newdata;
	CharPtr->pitch = dpitch;
	CharPtr->disp.width = CharPtr->extent.width;
	CharPtr->disp.height = CharPtr->extent.height;
	CharPtr->HotSpot.x = 0;
	
	{
		// This tunes the font positioning to be about what it should
		// TODO: prolly needs a little tweaking still

		int tune_amount = 0;

		if (CharPtr->disp.height <= RES_SCALE (8))
			tune_amount = RES_SCALE (1);
		else if (CharPtr->disp.height == RES_SCALE (9))
			tune_amount = RES_SCALE (2);
		else if (CharPtr->disp.height > RES_SCALE (9))
			tune_amount = RES_SCALE (3);

		if (!fontPtr->HaveFntData)
			fontPtr->VertAlign = tune_amount;

		CharPtr->HotSpot.y = CharPtr->disp.height - fontPtr->VertAlign;
	}
}

void *
_GetCelData (uio_Stream *fp, DWORD length)
{
	int cel_total, cel_index, n;
	DWORD opos;
	char CurrentLine[1024], filename[PATH_MAX];
	TFB_Canvas *img;
	AniData *ani;
	DRAWABLE Drawable;
	uio_MountHandle *aniMount = 0;
	uio_DirHandle *aniDir = 0;
	uio_Stream *aniFile = 0;
	
	opos = uio_ftell (fp);

	{
		char *s1, *s2;
		char aniDirName[PATH_MAX];
		const char *aniFileName;
		uint8 buf[4] = { 0, 0, 0, 0 };
		uint32 header;

		if (_cur_resfile_name == 0
				|| (((s2 = 0), (s1 = strrchr (_cur_resfile_name, '/')) == 0)
						&& (s2 = strrchr (_cur_resfile_name, '\\')) == 0))
		{
			n = 0;
		}
		else
		{
			if (s2 > s1)
				s1 = s2;
			n = s1 - _cur_resfile_name + 1;
		}

		uio_fread(buf, 4, 1, fp);
		header = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
 		if (_cur_resfile_name && header == 0x04034b50)
		{
			// zipped ani file
			if (n)
			{
				strncpy (aniDirName, _cur_resfile_name, n - 1);
				aniDirName[n - 1] = 0;
				aniFileName = _cur_resfile_name + n;
			}
			else
			{
				strcpy(aniDirName, ".");
				aniFileName = _cur_resfile_name;
			}
			aniDir = uio_openDir (repository, aniDirName, 0);
			aniMount = uio_mountDir (repository, aniDirName, uio_FSTYPE_ZIP,
							aniDir, aniFileName, "/", autoMount,
							uio_MOUNT_RDONLY | uio_MOUNT_TOP,
							NULL);
			aniFile = uio_fopen (aniDir, aniFileName, "r");
			opos = 0;
			n = 0;
		}
		else
		{
			// unpacked ani file
			strncpy (filename, _cur_resfile_name, n);
			aniFile = fp;
			aniDir = contentDir;
		}
	}

	cel_total = 0;
	uio_fseek (aniFile, opos, SEEK_SET);
	while (uio_fgets (CurrentLine, sizeof (CurrentLine), aniFile))
	{
		++cel_total;
	}

	img = HMalloc (sizeof (TFB_Canvas) * cel_total);
	ani = HMalloc (sizeof (AniData) * cel_total);
	if (!img || !ani)
	{
		log_add (log_Warning, "Couldn't allocate space for '%s'", _cur_resfile_name);
		if (aniMount)
		{
			uio_fclose(aniFile);
			uio_closeDir(aniDir);
			uio_unmountDir(aniMount);
		}
		HFree (img);
		HFree (ani);
		return NULL;
	}

	cel_index = 0;
	uio_fseek (aniFile, opos, SEEK_SET);
	while (uio_fgets (CurrentLine, sizeof (CurrentLine), aniFile) && cel_index < cel_total)
	{
		if (sscanf (CurrentLine, "%s %d %d %d %d", &filename[n],
				&ani[cel_index].transparent_color,
				&ani[cel_index].colormap_index,
				&ani[cel_index].hotspot_x, &ani[cel_index].hotspot_y) != 5)
			break;
	
		img[cel_index] = TFB_DrawCanvas_LoadFromFile (aniDir, filename);
		if (img[cel_index] == NULL)
		{
			const char *err;

			err = TFB_DrawCanvas_GetError ();
			log_add (log_Warning, "_GetCelData: Unable to load image!");
			if (err != NULL)
				log_add (log_Warning, "Gfx Driver reports: %s", err);
		}
		else
		{
			++cel_index;
		}

		if ((int)uio_ftell (aniFile) - (int)opos >= (int)length)
			break;
	}

	Drawable = NULL;
	if (cel_index && (Drawable = AllocDrawable (cel_index)))
	{
		if (!Drawable)
		{
			while (cel_index--)
				TFB_DrawCanvas_Delete (img[cel_index]);

			HFree (Drawable);
			Drawable = NULL;
		}
		else
		{
			FRAME FramePtr;

			Drawable->Flags = WANT_PIXMAP;
			Drawable->MaxIndex = cel_index - 1;

			FramePtr = &Drawable->Frame[cel_index];
			while (--FramePtr, cel_index--)
				process_image (FramePtr, img, ani, cel_index);
		}
	}

	if (Drawable == NULL)
		log_add (log_Warning, "Couldn't get cel data for '%s'",
				_cur_resfile_name);

	if (aniMount)
	{
		uio_fclose(aniFile);
		uio_closeDir(aniDir);
		uio_unmountDir(aniMount);
	}

	HFree (img);
	HFree (ani);
	return Drawable;
}

BOOLEAN
_ReleaseCelData (void *handle)
{
	DRAWABLE DrawablePtr;
	int cel_ct;
	FRAME FramePtr = NULL;

	if ((DrawablePtr = handle) == 0)
		return (FALSE);

	cel_ct = DrawablePtr->MaxIndex + 1;
	FramePtr = DrawablePtr->Frame;

	HFree (handle);
	if (FramePtr)
	{
		int i;
		for (i = 0; i < cel_ct; i++)
		{
			TFB_Image *img = FramePtr[i].image;
			if (img)
			{
				FramePtr[i].image = NULL;
				TFB_DrawScreen_DeleteImage (img);
			}
		}
		HFree (FramePtr);
	}

	return (TRUE);
}

typedef struct BuildCharDesc
{
	TFB_Canvas canvas;
	UniChar index;
} BuildCharDesc;

static int
compareBCDIndex (const void *arg1, const void *arg2)
{
	const BuildCharDesc *bcd1 = (const BuildCharDesc *) arg1;
	const BuildCharDesc *bcd2 = (const BuildCharDesc *) arg2;

	return (int) bcd1->index - (int) bcd2->index;
}

// A font directory may contain a file 'fallback.fnt', holding the
// content path of another font (e.g. "base/fonts/cjk-12.fon"),
// optionally followed by a VertAlign value for it:
//     addons/mm-hd/fonts/cjk-36.fon 9
// Characters missing from the font are then taken from that fallback
// font. This lets every font share one set of CJK glyphs; the VertAlign
// value lines the fallback glyphs up with the font's own baseline, so
// fonts of the same size but different baselines can share them too.
// Fallback fonts are loaded once per path and VertAlign and shared
// between all fonts that refer to them; they have no fallbacks of their
// own.
#define NO_VERT_ALIGN 1000
		// fallback.fnt gave no VertAlign; use the fallback font's own

typedef struct FallbackFont
{
	struct FallbackFont *next;
	char *path;
	int vertAlign;
	FONT font;
	int refCount;
} FallbackFont;

static FallbackFont *fallbackFonts = NULL;
static BOOLEAN loadingFallback = FALSE;
static int fallbackVertAlign = NO_VERT_ALIGN;
		// VertAlign for the fallback font being loaded

static FONT
acquireFallbackFont (const char *path, int vertAlign)
{
	FallbackFont *entry;
	const char *savedResName;
	FONT font;

	for (entry = fallbackFonts; entry != NULL; entry = entry->next)
	{
		if (strcmp (entry->path, path) == 0
				&& entry->vertAlign == vertAlign)
		{
			entry->refCount++;
			return entry->font;
		}
	}

	// LoadResourceFromPath() clears _cur_resfile_name when done
	savedResName = _cur_resfile_name;
	loadingFallback = TRUE;
	fallbackVertAlign = vertAlign;
	font = LoadResourceFromPath (path, _GetFontData);
	loadingFallback = FALSE;
	fallbackVertAlign = NO_VERT_ALIGN;
	_cur_resfile_name = savedResName;

	if (font == NULL)
	{
		log_add (log_Warning, "Warning: Can't load fallback font '%s' "
				"for font '%s'.", path, _cur_resfile_name);
		return NULL;
	}

	entry = HMalloc (sizeof *entry);
	entry->path = HMalloc (strlen (path) + 1);
	strcpy (entry->path, path);
	entry->vertAlign = vertAlign;
	entry->font = font;
	entry->refCount = 1;
	entry->next = fallbackFonts;
	fallbackFonts = entry;

	return font;
}

static void
releaseFallbackFont (FONT font)
{
	FallbackFont **link;

	for (link = &fallbackFonts; *link != NULL; link = &(*link)->next)
	{
		FallbackFont *entry = *link;

		if (entry->font != font)
			continue;

		if (--entry->refCount == 0)
		{
			*link = entry->next;
			_ReleaseFontData (entry->font);
			HFree (entry->path);
			HFree (entry);
		}
		return;
	}
}

// Reads the fallback font path and VertAlign from 'fallback.fnt'.
// 'path' is made empty if there is no such file.
static void
readFallbackFontPath (uio_DirHandle *fontDirHandle, const char *fileName,
		char *path, int *vertAlign)
{
	uio_Stream *file;
	char line[PATH_MAX + 16];
	char format[32];
	int fields;

	path[0] = '\0';
	*vertAlign = NO_VERT_ALIGN;

	file = uio_fopen (fontDirHandle, fileName, "r");
	if (file == NULL)
		return;

	if (uio_fgets (line, sizeof line, file) != NULL)
	{
		// 'path' holds PATH_MAX characters including the terminator
		snprintf (format, sizeof format, "%%%ds %%d", PATH_MAX - 1);
		fields = sscanf (line, format, path, vertAlign);
		if (fields < 1)
			path[0] = '\0';
		if (fields < 2)
			*vertAlign = NO_VERT_ALIGN;
	}
	uio_fclose (file);
}

void *
_GetFontData (uio_Stream *fp, DWORD length)
{
	COUNT numDirEntries;
	DIRENTRY fontDir = NULL;
	BuildCharDesc *bcds = NULL;
	size_t numBCDs = 0;
	int dirEntryI;
	uio_DirHandle *fontDirHandle = NULL;
	uio_MountHandle *fontMount = NULL;
	FONT fontPtr = NULL;
	uio_Stream *cfgFile = 0;
	const char *cfg_name = "kerndat.fnt";
	const char *fallback_name = "fallback.fnt";
	char fallbackPath[PATH_MAX];
	int fallbackVertAlignValue = NO_VERT_ALIGN;

	if (_cur_resfile_name == 0)
		goto err;

	if (fp != (uio_Stream*)~0)
	{
		// font is zipped instead of being in a directory

		char *s1, *s2;
		int n;
		const char *fontZipName;
		char fontDirName[PATH_MAX];

		if ((((s2 = 0), (s1 = strrchr (_cur_resfile_name, '/')) == 0)
						&& (s2 = strrchr (_cur_resfile_name, '\\')) == 0))
		{
			strcpy(fontDirName, ".");
			fontZipName = _cur_resfile_name;
		}
		else
		{
			if (s2 > s1)
				s1 = s2;
			n = s1 - _cur_resfile_name + 1;
			strncpy (fontDirName, _cur_resfile_name, n - 1);
			fontDirName[n - 1] = 0;
			fontZipName = _cur_resfile_name + n;
		}

		fontDirHandle = uio_openDir (repository, fontDirName, 0);
		fontMount = uio_mountDir (repository, _cur_resfile_name, uio_FSTYPE_ZIP,
						fontDirHandle, fontZipName, "/", autoMount,
						uio_MOUNT_RDONLY | uio_MOUNT_TOP,
						NULL);
		uio_closeDir (fontDirHandle);
	}

	fontDir = CaptureDirEntryTable (LoadDirEntryTable (contentDir,
			_cur_resfile_name, ".", match_MATCH_SUBSTRING));
	if (fontDir == 0)
		goto err;
	numDirEntries = GetDirEntryTableCount (fontDir);

	fontDirHandle = uio_openDirRelative (contentDir, _cur_resfile_name, 0);
	if (fontDirHandle == NULL)
		goto err;
		
	bcds = HMalloc (numDirEntries * sizeof (BuildCharDesc));
	if (bcds == NULL)
		goto err;

	// Load the surfaces for all dir Entries
	for (dirEntryI = 0; dirEntryI < numDirEntries; dirEntryI++)
	{
		char *char_name;
		unsigned int charIndex;
		TFB_Canvas canvas;
		EXTENT size;

		char_name = GetDirEntryAddress (SetAbsDirEntryTableIndex (
				fontDir, dirEntryI));

		if (strcmp (char_name, cfg_name) == 0
				|| strcmp (char_name, fallback_name) == 0)
			continue;

		if (sscanf (char_name, "%x.", &charIndex) != 1)
			continue;
			
		if (charIndex > MAX_UNICODE)
			continue;

		canvas = TFB_DrawCanvas_LoadFromFile (fontDirHandle, char_name);
		if (canvas == NULL)
			continue;

		TFB_DrawCanvas_GetExtent (canvas, &size);
		if (size.width == 0 || size.height == 0)
		{
			TFB_DrawCanvas_Delete (canvas);
			continue;
		}

		bcds[numBCDs].canvas = canvas;
		bcds[numBCDs].index = charIndex;
		numBCDs++;
	}

	fontPtr = AllocFont (0);
	if (fontPtr == NULL)
		goto err;

	cfgFile = uio_fopen (fontDirHandle, cfg_name, "r");

	if (cfgFile)
	{
		char CurrentLine[PATH_MAX];
		int cel_index = 0;
		int cel_total = 0;
		DWORD opos = 0;

		memset (&fontPtr->KernTab, (BYTE)~0, sizeof (fontPtr->KernTab));

		uio_fseek (cfgFile, opos, SEEK_SET);
		while (uio_fgets (CurrentLine, sizeof (CurrentLine), cfgFile))
		{
			++cel_total;
			if (cel_total == MAX_UNICODE)
				break;
		}

		uio_fseek (cfgFile, opos, SEEK_SET);
		while (uio_fgets (
				CurrentLine, sizeof (CurrentLine),
				cfgFile) && cel_index < cel_total)
		{
			if (cel_index > 0)
			{
				SDWORD KernChar, kernLBits, kernRBits;

				if (sscanf (CurrentLine, "%x %u %u", &KernChar,
						&kernLBits, &kernRBits) == 3)
				{
					if (kernLBits > 3 || kernLBits < 0)
						kernLBits = 3;
					if (kernRBits > 3 || kernRBits < 0)
						kernRBits = 3;

					fontPtr->KernTab[KernChar] =
							(kernLBits << 2) | kernRBits;
				}
			}
			else
			{
				if (sscanf (CurrentLine, "%s %hhu %hhu %hhu %hhd",
						fontPtr->filename, &fontPtr->Leading,
						&fontPtr->CharSpace, &fontPtr->KernAmount,
						&fontPtr->VertAlign) == 5)
				{
					fontPtr->HaveFntData = TRUE;
				}
				else
					break;
			}

			++cel_index;
			if (cel_index == MAX_UNICODE)
				break;

			if ((int)uio_ftell (cfgFile) - (int)opos
					>= (int)LengthResFile (cfgFile))
				break;
		}
		uio_fclose (cfgFile);
	}

	if (loadingFallback)
	{
		fallbackPath[0] = '\0';
		if (fallbackVertAlign != NO_VERT_ALIGN)
		{	// Line the glyphs up with the font that uses this fallback
			fontPtr->VertAlign = (SBYTE) fallbackVertAlign;
			fontPtr->HaveFntData = TRUE;
		}
	}
	else
		readFallbackFontPath (fontDirHandle, fallback_name, fallbackPath,
				&fallbackVertAlignValue);

	uio_closeDir (fontDirHandle);
	DestroyDirEntryTable (ReleaseDirEntryTable (fontDir));
	if (fontMount != 0)
		uio_unmountDir (fontMount);

#if 0
	if (numBCDs == 0)
		goto err;
#endif

	// sort on the character index
	qsort (bcds, numBCDs, sizeof (BuildCharDesc), compareBCDIndex);

	{
		size_t startBCD = 0;
		UniChar pageStart;
		FONT_PAGE **pageEndPtr = &fontPtr->fontPages;
		while (startBCD < numBCDs)
		{
			// Process one character page.
			size_t endBCD;
			pageStart = bcds[startBCD].index & CHARACTER_PAGE_MASK;

			endBCD = startBCD;
			while (endBCD < numBCDs &&
					(bcds[endBCD].index & CHARACTER_PAGE_MASK) == pageStart)
				endBCD++;

			{
				size_t bcdI;
				int numChars = bcds[endBCD - 1].index + 1
						- bcds[startBCD].index;
				FONT_PAGE *page = AllocFontPage (numChars);
				page->pageStart = pageStart;
				page->firstChar = bcds[startBCD].index;
				page->numChars = numChars;
				*pageEndPtr = page;
				pageEndPtr = &page->next;

				for (bcdI = startBCD; bcdI < endBCD; bcdI++)
				{
					// Process one character.
					BuildCharDesc *bcd = &bcds[bcdI];
					TFB_Char *destChar =
							&page->charDesc[bcd->index - page->firstChar];
				
					if (destChar->data != NULL)
					{
						// There's already an image for this character.
						log_add (log_Debug, "Duplicate image for character %d "
								"for font %s.", (int) bcd->index,
								_cur_resfile_name);
						TFB_DrawCanvas_Delete (bcd->canvas);
						continue;
					}
					
					processFontChar (destChar, bcd->canvas, fontPtr);
					TFB_DrawCanvas_Delete (bcd->canvas);

					if (destChar->disp.height > fontPtr->disp.height)
						fontPtr->disp.height = destChar->disp.height;
					if (destChar->disp.width > fontPtr->disp.width)
						fontPtr->disp.width = destChar->disp.width;
				}
			}

			startBCD = endBCD;
		}
		*pageEndPtr = NULL;
	}

	if (!fontPtr->HaveFntData)
	{
		fontPtr->Leading = fontPtr->disp.height + RES_SCALE (1);
		fontPtr->CharSpace = RES_SCALE (1);
	}

	HFree (bcds);

	// Load the fallback font last; this loads another resource and
	// _cur_resfile_name (and any zip mount) is no longer needed here.
	if (fallbackPath[0] != '\0')
		fontPtr->Fallback = acquireFallbackFont (fallbackPath,
				fallbackVertAlignValue);

	(void) fp;  /* Satisfying compiler (unused parameter) */
	(void) length;  /* Satisfying compiler (unused parameter) */
	return fontPtr;

err:
	if (fontPtr != 0)
		HFree (fontPtr);
	
	if (bcds != NULL)
	{
		size_t bcdI;
		for (bcdI = 0; bcdI < numBCDs; bcdI++)
			TFB_DrawCanvas_Delete (bcds[bcdI].canvas);
		HFree (bcds);
	}
	
	if (fontDirHandle != NULL)
		uio_closeDir (fontDirHandle);
	
	if (fontDir != 0)
		DestroyDirEntryTable (ReleaseDirEntryTable (fontDir));

	if (fontMount != 0)
		uio_unmountDir(fontMount);

	return 0;
}

BOOLEAN
_ReleaseFontData (void *handle)
{
	FONT font = (FONT) handle;
	if (font == NULL)
		return FALSE;

	if (font->Fallback != NULL)
		releaseFallbackFont (font->Fallback);

	{
		FONT_PAGE *page;
		FONT_PAGE *nextPage;

		for (page = font->fontPages; page != NULL; page = nextPage)
		{
			size_t charI;
			for (charI = 0; charI < page->numChars; charI++)
			{
				TFB_Char *c = &page->charDesc[charI];
				
				if (c->data == NULL)
					continue;
				
				// XXX: fix this if fonts get per-page data
				//  rather than per-char
				TFB_DrawScreen_DeleteData (c->data);
			}
		
			nextPage = page->next;
			FreeFontPage (page);
		}
	}

	HFree (font);

	return TRUE;
}
