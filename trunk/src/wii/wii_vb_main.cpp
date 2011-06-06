/*
WiirtualBoy : Wii port of the Mednafen Virtual Boy emulator

Copyright (C) 2011
raz0red (www.twitchasylum.com)

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1.	The origin of this software must not be misrepresented; you
must not claim that you wrote the original software. If you use
this software in a product, an acknowledgment in the product
documentation would be appreciated but is not required.

2.	Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3.	This notice may not be removed or altered from any source
distribution.
*/

#include "main.h"
#include "sound.h"

#include "wii_app.h"
#include "wii_gx.h"
#include "wii_main.h"
#include "wii_sdl.h"

#include "wii_vb.h"
#include "wii_Vb_input.h"
#include "wii_vb_main.h"
#include "wii_vb_sdl.h"

#ifdef WII_NETTRACE
#include <network.h>
#include "net_print.h"  
#endif

extern "C" 
{
void WII_VideoStart();
void WII_VideoStop();
void WII_ChangeSquare(int xscale, int yscale, int xshift, int yshift);
void WII_SetRenderCallback( void (*cb)(void) );
Mtx gx_view;
}

// Forward references
static void gxrender_callback();

// Mednafen external references
extern volatile Uint32 MainThreadID;
extern MDFNSetting DriverSettings[]; 
extern int DriverSettingsSize;
extern std::vector <MDFNSetting> NeoDriverSettings;
extern char *DrBaseDirectory;
extern volatile MDFN_Surface *VTReady;
extern volatile MDFN_Rect *VTLWReady;
extern volatile MDFN_Rect *VTDRReady;
extern MDFN_Rect VTDisplayRects[2];
extern volatile MDFN_Surface *VTBuffer[2];
extern MDFN_Rect *VTLineWidths[2];
extern SDL_Surface *screen;
extern volatile int NeedVideoChange;

namespace MDFN_IEN_VB
{
  extern void VIP_SetParallaxDisable(bool disabled);
  extern void VIP_SetAnaglyphColors(uint32 lcolor, uint32 rcolor);
  extern void VIP_Set3DMode(uint32 mode, bool reverse, uint32 prescale, uint32 sbs_separation);
}

extern int LoadGame(const char *force_module, const char *path);
extern int GameLoop(void *arg);
extern char *GetBaseDirectory(void);
extern void KillVideo(void);
extern void FPS_Init(void);
extern void MakeMednafenArgsStruct(void);
extern bool CreateDirs(void);
extern void MakeVideoSettings(std::vector <MDFNSetting> &settings);
extern void MakeInputSettings(std::vector <MDFNSetting> &settings);
extern void DeleteInternalArgs(void);
extern void KillInputSettings(void);
extern void CalcFramerates(char *virtfps, char *drawnfps, char *blitfps, size_t maxlen);

/*
 * Initializes the emulator
 */
void wii_vb_init()
{
  std::vector<MDFNGI *> ExternalSystems;
  MainThreadID = SDL_ThreadID();

  DrBaseDirectory=GetBaseDirectory();

  MDFNI_printf(_("Starting Mednafen %s\n"), MEDNAFEN_VERSION);
  MDFN_indent(1);
  MDFN_printf(_("Base directory: %s\n"), DrBaseDirectory);

  // Look for external emulation modules here.
  if(!MDFNI_InitializeModules(ExternalSystems))
  {
    MDFN_PrintError( "Unable to initialize external modules" );
    exit( 0 );
  }

  for(unsigned int x = 0; x < DriverSettingsSize / sizeof(MDFNSetting); x++)
    NeoDriverSettings.push_back(DriverSettings[x]);

  MakeVideoSettings(NeoDriverSettings);
  MakeInputSettings(NeoDriverSettings);

  if(!(MDFNI_Initialize(DrBaseDirectory, NeoDriverSettings))) 
  {
    MDFN_PrintError( "Error during initialization" );
    exit( 0 );
  }

  //
  // TODO: I don't think we need to create dirs...
  //
  if(!CreateDirs())
  {
    ErrnoHolder ene(errno);	// TODO: Maybe we should have CreateDirs() return this instead?

    MDFN_PrintError(_("Error creating directories: %s\n"), ene.StrError());
    exit( 0 );
  }

  MakeMednafenArgsStruct();

  VTReady = NULL;
  VTDRReady = NULL;
  VTLWReady = NULL;

  MDFN_PixelFormat nf;
#if BPP == 8
  nf.bpp = 8;
#elif BPP == 16
  nf.bpp = 16;
#else
  nf.bpp = 32;
#endif
  nf.colorspace = MDFN_COLORSPACE_RGB;

  VTBuffer[0] = new MDFN_Surface(NULL, VB_WIDTH, VB_HEIGHT, VB_WIDTH, nf);
  VTBuffer[1] = new MDFN_Surface(NULL, VB_WIDTH, VB_HEIGHT, VB_WIDTH, nf);
  VTLineWidths[0] = (MDFN_Rect *)calloc(VB_HEIGHT, sizeof(MDFN_Rect));
  VTLineWidths[1] = (MDFN_Rect *)calloc(VB_HEIGHT, sizeof(MDFN_Rect));

  FPS_Init();

  KillVideo();

  // Set the screen to our back surface
  screen = back_surface;  
}

/*
 * Free resources (closes) the emulator
 */
void wii_vb_free()
{
  CloseGame();

  for(int x = 0; x < 2; x++)
  {
    if(VTBuffer[x])
    {
      delete VTBuffer[x];
      VTBuffer[x] = NULL;
    }

    if(VTLineWidths[x])
    {
      free(VTLineWidths[x]);
      VTLineWidths[x] = NULL;
    }
  }

  MDFNI_Kill();
  DeleteInternalArgs();
  KillInputSettings();
}

/*
 * Loads the specified game
 *
 * game     The name of the game
 * return   1 if the load is successful, 0 if it fails
 */
int wii_vb_load_game( char* game )
{
  return LoadGame( NULL, game );
}

/*
 * The emulation loop
 */
void wii_vb_emu_loop()
{
  for(int i = 0; i < 2; i++)
    ((MDFN_Surface *)VTBuffer[i])->Fill(0, 0, 0, 0);

  Vb3dMode mode = wii_get_vb_mode();
  MDFN_IEN_VB::VIP_SetParallaxDisable( !mode.isParallax );
  MDFN_IEN_VB::VIP_SetAnaglyphColors( mode.lColor, mode.rColor );
  //MDFN_IEN_VB::VIP_Set3DMode( 0, false, 1, 0 );

  wii_sdl_black_back_surface();
  WII_SetRenderCallback( &gxrender_callback );  
  WII_ChangeSquare( wii_screen_x, wii_screen_y, 0, 0 );  
  WII_VideoStart();    
  ClearSound();
  PauseSound( 0 );

  GameThreadRun = 1;
  NeedVideoChange = 0;

  GameLoop( NULL );

  PauseSound( 1 );
  WII_VideoStop();     
}

#define CB_PIXELSIZE 14
#define CB_H CB_PIXELSIZE
#define CB_PADDING 2
#define CB_X -310
#define CB_Y 196

/*
 * GX render callback
 */
static void gxrender_callback()
{
  static int callback = 0;

  GX_SetVtxDesc( GX_VA_POS, GX_DIRECT );
  GX_SetVtxDesc( GX_VA_CLR0, GX_DIRECT );
  GX_SetVtxDesc( GX_VA_TEX0, GX_NONE );

  Mtx m;    // model matrix.
  Mtx mv;   // modelview matrix.

  guMtxIdentity( m ); 
  guMtxTransApply( m, m, 0, 0, -100 );
  guMtxConcat( gx_view, m, mv );
  GX_LoadPosMtxImm( mv, GX_PNMTX0 ); 

  if( wii_debug )
  {    
    static char virtfps[64];
    static char drawnfps[64];
    static char blitfps[64];
    static char text[256] = "";

    if( callback++ % 60 == 0 )
    {
      CalcFramerates( virtfps, drawnfps, blitfps, 64 );  
    }
    sprintf( text, "%s %s %s", virtfps, drawnfps, blitfps );

    GXColor color = (GXColor){0x0, 0x0, 0x0, 0x80};                       
    wii_gx_drawrectangle( 
      CB_X + -CB_PADDING, 
      CB_Y + CB_H + CB_PADDING, 
      wii_gx_gettextwidth( CB_PIXELSIZE, text ) + (CB_PADDING<<1), 
      CB_H + (CB_PADDING<<1), 
      color, TRUE );

    wii_gx_drawtext( CB_X, CB_Y, CB_PIXELSIZE, text, ftgxWhite, FTGX_ALIGN_BOTTOM ); 
  }
}