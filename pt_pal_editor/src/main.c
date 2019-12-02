// for finding memory leaks in debug mode with Visual Studio 
#if defined _DEBUG && defined _MSC_VER
#include <crtdbg.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <sys/stat.h>
#ifdef _WIN32
#define WIN32_MEAN_AND_LEAN
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <SDL2/SDL.h>
#include "config.h"
#include "palette.h"
#include "gui.h"

#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))

#ifdef _MSC_VER
#pragma warning(disable: 4204)
#pragma warning(disable: 4221)
#endif

volatile bool programRunning, redrawScreen, allowSigterm = true;
uint32_t *frameBuffer;

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;

static bool setupVideo(void);
static void readMouseXY(void);
static void handleInput(void);

#ifdef __APPLE__
static void osxSetDirToProgramDirFromArgs(char **argv);
#endif

int main(int argc, char *argv[])
{
	// for finding memory leaks in debug mode with Visual Studio
#if defined _DEBUG && defined _MSC_VER
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

	(void)argc;
	(void)argv;

#ifdef __APPLE__
	osxSetDirToProgramDirFromArgs(argv);
#endif

	if (!setupVideo())
	{
		SDL_Quit();
		return 0;
	}

	setupGUI();
	redrawScreen = true;

	programRunning = true;
	while (programRunning)
	{
		readMouseXY();
		handleInput();

		if (redrawScreen)
		{
			redrawScreen = false;

			// redraw screen
			SDL_UpdateTexture(texture, NULL, frameBuffer, SCREEN_W * sizeof (int32_t));
			SDL_RenderClear(renderer);
			SDL_RenderCopy(renderer, texture, NULL, NULL);
			SDL_RenderPresent(renderer);
		}

		SDL_Delay((1000 / 60) + 1); // ~60Hz
	}

	if (loadedFile != NULL) free(loadedFile);
	free(frameBuffer);
	SDL_Quit();

	return 0;
}

static void readMouseXY(void)
{
	int32_t mx, my;

	SDL_PumpEvents();
	SDL_GetMouseState(&mx, &my);

	if (mx < 0) mx = 0;
	if (my < 0) my = 0;

	mx = (((uint32_t)mx * input.mouse.xScaleMul) + (1 << 15)) >> 16;
	my = (((uint32_t)my * input.mouse.yScaleMul) + (1 << 15)) >> 16;

	if (mx >= SCREEN_W) mx = SCREEN_W - 1;
	if (my >= SCREEN_H) my = SCREEN_H - 1;

	input.mouse.x = (int16_t)mx;
	input.mouse.y = (int16_t)my;
}

static void showAskToSaveDialog(void)
{
	int32_t whichButtonPressed;

	const SDL_MessageBoxButtonData buttons[2] =
	{
		{ SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"  },
		{ SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" },
	};

	const SDL_MessageBoxData messageBoxData =
	{
		SDL_MESSAGEBOX_WARNING,
		window,
		"Warning",
		"Colors are unsaved. Save before quitting?",
		SDL_arraysize(buttons),
		buttons,
		NULL
	};

	if (SDL_ShowMessageBox(&messageBoxData, &whichButtonPressed) < 0)
	{
		allowSigterm = true;
		programRunning = false;
		return;
	}

	if (whichButtonPressed == 1)
		savePalette(0);

	programRunning = false;
}

void showErrorMsgBox(const char *fmt, ...)
{
	char strBuf[1024];
	va_list args;

	// format the text string
	va_start(args, fmt);
	vsnprintf(strBuf, sizeof (strBuf), fmt, args);
	va_end(args);

	// window can be NULL here, no problem...
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", strBuf, NULL);
}

static void handleInput(void)
{
	SDL_Event inputEvent;

	while (SDL_PollEvent(&inputEvent))
	{
		if (inputEvent.type == SDL_QUIT && allowSigterm)
		{
			if (!configIsSaved)
			{
				allowSigterm = false;
				showAskToSaveDialog();
			}
			else
			{
				programRunning = false;
			}
		}
		else if (inputEvent.type == SDL_KEYDOWN)
		{
			keyDownHandler(inputEvent.key.keysym.sym);
		}
		else if (inputEvent.type == SDL_MOUSEBUTTONUP)
		{
			if (inputEvent.button.button == SDL_BUTTON_LEFT)
				mouseButtonUpHandler();
		}
		else if (inputEvent.type == SDL_MOUSEBUTTONDOWN)
		{
			if (inputEvent.button.button == SDL_BUTTON_LEFT)
				mouseButtonDownHandler();
		}
	}

	handleSlidersHeldDown();
	handleRainbowHeldDown();
	handleRainbowUpDownButtons();
}

// macOS/OS X specific routines
#ifdef __APPLE__
static void osxSetDirToProgramDirFromArgs(char **argv)
{
	char *tmpPath;
	int32_t i, tmpPathLen;

	/* OS X/macOS: hackish way of setting the current working directory to the place where we double clicked
	** on the icon (for protracker.ini loading) */

	// if we launched from the terminal, argv[0][0] would be '.'
	if (argv[0] != NULL && argv[0][0] == '/') // don't do the hack if we launched from the terminal
	{
		tmpPath = strdup(argv[0]);
		if (tmpPath != NULL)
		{
			// cut off program filename
			tmpPathLen = strlen(tmpPath);
			for (i = tmpPathLen-1; i >= 0; i--)
			{
				if (tmpPath[i] == '/')
				{
					tmpPath[i] = '\0';
					break;
				}
			}

			chdir(tmpPath); // path to binary
			chdir("../../../"); // we should now be in the directory where the config can be.

			free(tmpPath);
		}
	}
}
#endif

static bool setupVideo(void)
{
	int32_t w, h;
	double dScaleX, dScaleY;

	/* SDL 2.0.9 for Windows has a serious bug where you need to initialize the joystick subsystem
	** (even if you don't use it) or else weird things happen like random stutters, keyboard (rarely) being
	** reinitialized in Windows and what not.
	** Ref.: https://bugzilla.libsdl.org/show_bug.cgi?id=4391 */
#if defined _WIN32 && SDL_PATCHLEVEL == 9
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0)
#else
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
#endif
	{
		showErrorMsgBox("Couldn't initialize SDL: %s", SDL_GetError());
		return false;
	}

	window = SDL_CreateWindow("Palette editor for ProTracker 2.3D clone",
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W * 2, SCREEN_H * 2, SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);

	if (window == NULL)
	{
		showErrorMsgBox("Couldn't create an SDL2 window: %s", SDL_GetError());
		return false;
	}

	renderer = SDL_CreateRenderer(window, -1, 0);
	if (renderer == NULL)
	{
		showErrorMsgBox("Couldn't create an SDL2 renderer: %s", SDL_GetError());
		return false;
	}

	SDL_RenderSetLogicalSize(renderer, SCREEN_W, SCREEN_H);

#if SDL_PATCHLEVEL >= 5
	SDL_RenderSetIntegerScale(renderer, SDL_TRUE);
#endif

	SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

	SDL_SetHint("SDL_RENDER_SCALE_QUALITY", "nearest");

	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
	if (texture == NULL)
	{
		showErrorMsgBox("Couldn't create an SDL2 texture: %s", SDL_GetError());
		return false;
	}

	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);

	// frame buffer used by SDL (for texture)
	frameBuffer = (uint32_t *)malloc(SCREEN_W * SCREEN_H * sizeof (int32_t));
	if (frameBuffer == NULL)
	{
		showErrorMsgBox("Out of memory!");
		return false;
	}

	SDL_GetWindowSize(window, &w, &h);

	dScaleX = w / (double)SCREEN_W;
	dScaleY = h / (double)SCREEN_H;

	input.mouse.xScaleMul = (dScaleX == 0.0) ? 65536 : (uint32_t)round(65536.0 / dScaleX);
	input.mouse.yScaleMul = (dScaleY == 0.0) ? 65536 : (uint32_t)round(65536.0 / dScaleY);

	return true;
}
