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

// Keeps Windows input methods (IMEs) away from the game window.
//
// SDL enables the IME whenever text input is on, which it is from start-up
// and while entering a name. With an East Asian IME such as Bopomofo
// active, typed keys go into the IME's composition string, which the game
// never shows: letters seem to vanish and Enter only commits the invisible
// composition, so the game cannot be controlled. Detaching the IME from
// the window makes keys reach the game directly; text input then produces
// whatever the keyboard layout types without an IME.

#include <SDL.h>
#include "ime.h"

#ifdef _WIN32

#include <SDL_syswm.h>
#include <windows.h>
#include <imm.h>

extern SDL_Window *window;

typedef BOOL (WINAPI *ImmAssociateContextExFn) (HWND, HIMC, DWORD);

void
TFB_DisableIME (void)
{
	// Loaded at run time so that no build needs to link imm32.
	static ImmAssociateContextExFn immAssociateContextEx = NULL;
	static BOOL loaded = FALSE;
	SDL_SysWMinfo info;

	if (!loaded)
	{
		HMODULE imm = LoadLibraryA ("imm32.dll");
		if (imm != NULL)
			immAssociateContextEx = (ImmAssociateContextExFn)
					GetProcAddress (imm, "ImmAssociateContextEx");
		loaded = TRUE;
	}

	if (immAssociateContextEx == NULL || window == NULL)
		return;

	SDL_VERSION (&info.version);
	if (!SDL_GetWindowWMInfo (window, &info))
		return;

	// A NULL input context detaches the IME from the window.
	immAssociateContextEx (info.info.win.window, NULL, 0);
}

#else

void
TFB_DisableIME (void)
{
}

#endif
