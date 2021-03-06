#include <assert.h>
#include <vector>
#include <list>

using namespace std;

#include "common.h"
#include "Tile.h"
#include "GUI.h"
//#include "SpriteMaps.h"
#include "GameBuildings.h"
#include "Constructions.h"
#include "DumpInfo.h"
#include "MapLoading.h"
#include "WorldSegment.h"
#include "Creatures.h"
#include "GroundMaterialConfiguration.h"
#include "ContentLoader.h"
#include "OcclusionTest.h"

#define WIDTH        640
#define HEIGHT       480
#define SIZE_LOG     50

#ifdef LINUX_BUILD
#include "stonesense.xpm"
extern void *allegro_icon;
#endif

bool stonesense_started = 0;

uint32_t DebugInt1;

int keyoffset=0;

GameConfiguration ssConfig;
GameState ssState;
FrameTimers ssTimers;

bool timeToReloadSegment;
bool timeToReloadConfig;
char currentAnimationFrame;
uint32_t currentFrameLong;
bool animationFrameShown;

vector<t_matgloss> v_stonetypes;

ALLEGRO_FONT * font;

ALLEGRO_DISPLAY * display;
ALLEGRO_KEYBOARD_STATE keyboard;

ALLEGRO_TIMER * reloadtimer;
ALLEGRO_TIMER * animationtimer;

ALLEGRO_EVENT event;

ALLEGRO_BITMAP* IMGIcon;

int mouse_x, mouse_y, mouse_z;
unsigned int mouse_b;
bool key[ALLEGRO_KEY_MAX];

/// main thread of stonesense - handles events
ALLEGRO_THREAD *stonesense_event_thread;
// the segment wrapper handles concurrency control
SegmentWrap map_segment;
bool redraw = true;

ALLEGRO_BITMAP* load_bitmap_withWarning(const char* path)
{
    ALLEGRO_BITMAP* img = 0;
    img = al_load_bitmap(path);
    if(!img) {
        LogError("Cannot load image: %s\n", path);
        al_set_thread_should_stop(stonesense_event_thread);
        return 0;
    }
    al_convert_mask_to_alpha(img, al_map_rgb(255, 0, 255));
    return img;
}


void LogError(const char* msg, ...)
{
    va_list arglist;
    va_start(arglist, msg);
    char buf[512] = {0};
    vsprintf(buf, msg, arglist);
    Core::printerr(buf);
    FILE* fp = fopen( "Stonesense.log", "a");
    if(fp) {
        vfprintf( fp, msg, arglist );
    }
//	Core::printerr(msg, arglist);
    va_end(arglist);
    fclose(fp);
}


void PrintMessage(const char* msg, ...)
{
    va_list arglist;
    va_start(arglist, msg);
    char buf[512] = {0};
    vsprintf(buf, msg, arglist);
    Core::print(buf);
    va_end(arglist);
}

void LogVerbose(const char* msg, ...)
{
    if (!ssConfig.verbose_logging) {
        return;
    }
    va_list arglist;
    va_start(arglist, msg);
    char buf[512] = {0};
    vsprintf(buf, msg, arglist);
    Core::printerr(buf);
    FILE* fp = fopen( "Stonesense.log", "a");
    if(fp) {
        vfprintf( fp, msg, arglist );
    }
//	Core::printerr(msg, arglist);
    va_end(arglist);
    fclose(fp);
}

void SetTitle(const char *format, ...)
{
    ALLEGRO_USTR *buf;
    va_list ap;
    const char *s;

    /* Fast path for common case. */
    if (0 == strcmp(format, "%s")) {
        va_start(ap, format);
        s = va_arg(ap, const char *);
        al_set_window_title(display, s);
        va_end(ap);
        return;
    }

    va_start(ap, format);
    buf = al_ustr_new("");
    al_ustr_vappendf(buf, format, ap);
    va_end(ap);

    al_set_window_title(display, al_cstr(buf));

    al_ustr_free(buf);
}

void correctTileForDisplayedOffset(int32_t& x, int32_t& y, int32_t& z)
{
    x -= ssState.DisplayedSegment.x;
    y -= ssState.DisplayedSegment.y; //DisplayedSegment.y;
    z -= ssState.DisplayedSegment.z - 1; // + viewedSegment->sizez - 2; // loading one above the top of the displayed segment for tile rules
}

bool loadfont(DFHack::color_ostream & output)
{
    ALLEGRO_PATH * p = al_create_path_for_directory("stonesense");
    if(!al_join_paths(p, ssConfig.font)) {
        al_destroy_path(p);
        return false;
    }
    font = al_load_font(al_path_cstr(p, ALLEGRO_NATIVE_PATH_SEP), ssConfig.fontsize, 0);
    if (!font) {
        output.printerr("Cannot load font: %s\n", al_path_cstr(p, ALLEGRO_NATIVE_PATH_SEP));
        al_destroy_path(p);
        return false;
    }
    al_destroy_path(p);
    return true;
}

void benchmark()
{
    ssState.DisplayedSegment.x = ssState.DisplayedSegment.y = 0;
    ssState.DisplayedSegment.x = 110;
    ssState.DisplayedSegment.y = 110;
    ssState.DisplayedSegment.z = 18;
    uint32_t startTime = clock();
    int i = 20;
    while(i--) {
        reloadDisplayedSegment();
    }

    FILE* fp = fopen("benchmark.txt", "w" );
    if(!fp) {
        return;
    }
    fprintf( fp, "%lims", clock() - startTime);
    fclose(fp);
}

void animUpdateProc()
{
    if (animationFrameShown) {
        // check before setting, or threadsafety will be borked
        if (currentAnimationFrame >= (MAX_ANIMFRAME-1)) { // ie ends up [0 .. MAX_ANIMFRAME)
            currentAnimationFrame = 0;
        } else {
            currentAnimationFrame++;
        }
        currentFrameLong++;
        animationFrameShown = false;
    }
}

void drawcredits()
{
    static ALLEGRO_BITMAP* SplashImage = NULL; // BUG: leaks the image
    al_clear_to_color(al_map_rgb(0,0,0));
    //centred splash image
    {
        if(!SplashImage) {
            ALLEGRO_PATH * p = al_create_path("stonesense/splash.png");
            SplashImage = load_bitmap_withWarning(al_path_cstr(p, ALLEGRO_NATIVE_PATH_SEP));
            al_destroy_path(p);
        }
        if(!SplashImage) {
            return;
        }
        al_draw_bitmap_region(SplashImage, 0, 0,
                              al_get_bitmap_width(SplashImage), al_get_bitmap_height(SplashImage),
                              (al_get_bitmap_width(al_get_backbuffer(al_get_current_display())) - al_get_bitmap_width(SplashImage))/2,
                              (al_get_bitmap_height(al_get_backbuffer(al_get_current_display())) - al_get_bitmap_height(SplashImage))/2, 0);
    }
    al_draw_text(font, al_map_rgb(255, 255, 0), al_get_bitmap_width(al_get_backbuffer(al_get_current_display()))/2, 5*al_get_font_line_height(font), ALLEGRO_ALIGN_CENTRE, "Welcome to Stonesense Felsite!");
    al_draw_text(font, al_map_rgb(255, 255, 255), al_get_bitmap_width(al_get_backbuffer(al_get_current_display()))/2, 6*al_get_font_line_height(font), ALLEGRO_ALIGN_CENTRE, "Stonesense is an isometric viewer for Dwarf Fortress.");

    al_draw_text(font, al_map_rgb(255, 255, 255), al_get_bitmap_width(al_get_backbuffer(al_get_current_display()))/2, 8*al_get_font_line_height(font), ALLEGRO_ALIGN_CENTRE, "Programming: Jonas Ask, Kris Parker, Japa Illo, Tim Aitken, and peterix");
    al_draw_text(font, al_map_rgb(255, 255, 255), al_get_bitmap_width(al_get_backbuffer(al_get_current_display()))/2, 9*al_get_font_line_height(font), ALLEGRO_ALIGN_CENTRE, "Lead graphics designer, Dale Holdampf");

    al_draw_text(font, al_map_rgb(255, 255, 255), al_get_bitmap_width(al_get_backbuffer(al_get_current_display()))/2, al_get_bitmap_height(al_get_backbuffer(al_get_current_display()))-13*al_get_font_line_height(font), ALLEGRO_ALIGN_CENTRE, "Contributors:");
    al_draw_text(font, al_map_rgb(255, 255, 255), al_get_bitmap_width(al_get_backbuffer(al_get_current_display()))/2, al_get_bitmap_height(al_get_backbuffer(al_get_current_display()))-12*al_get_font_line_height(font), ALLEGRO_ALIGN_CENTRE, "7c Nickel, BatCountry, Belal, Belannaer, DeKaFu, Dante, Deon, dyze,");
    al_draw_text(font, al_map_rgb(255, 255, 255), al_get_bitmap_width(al_get_backbuffer(al_get_current_display()))/2, al_get_bitmap_height(al_get_backbuffer(al_get_current_display()))-11*al_get_font_line_height(font), ALLEGRO_ALIGN_CENTRE, "Errol, fifth angel, frumpton, IDreamOfGiniCoeff, Impaler, ");
    al_draw_text(font, al_map_rgb(255, 255, 255), al_get_bitmap_width(al_get_backbuffer(al_get_current_display()))/2, al_get_bitmap_height(al_get_backbuffer(al_get_current_display()))-10*al_get_font_line_height(font), ALLEGRO_ALIGN_CENTRE, "Japa, jarathor, Jiri Petru, Jordix, Lord Nightmare, McMe, Mike Mayday, Nexii ");
    al_draw_text(font, al_map_rgb(255, 255, 255), al_get_bitmap_width(al_get_backbuffer(al_get_current_display()))/2, al_get_bitmap_height(al_get_backbuffer(al_get_current_display()))-9*al_get_font_line_height(font), ALLEGRO_ALIGN_CENTRE, "Malthus, peterix, Seuss, soup, Talvara, winner, Xandrin.");

    al_draw_text(font, al_map_rgb(255, 255, 255), al_get_bitmap_width(al_get_backbuffer(al_get_current_display()))/2, al_get_bitmap_height(al_get_backbuffer(al_get_current_display()))-7*al_get_font_line_height(font), ALLEGRO_ALIGN_CENTRE, "With special thanks to peterix for making dfHack");

    //"The program is in a very early alpha, we're only showcasing it to get ideas and feedback, so use it at your own risk."
    //al_draw_text(font, al_map_rgb(255, 255, 255), al_get_bitmap_width(al_get_backbuffer(al_get_current_display()))/2, al_get_bitmap_height(al_get_backbuffer(al_get_current_display()))-4*al_get_font_line_height(font), ALLEGRO_ALIGN_CENTRE, "Press F9 to continue");
    // Make the backbuffer visible
    al_flip_display();
}

/* main_loop:
*  The main loop of the program.  Here we wait for events to come in from
*  any one of the event sources and react to each one accordingly.  While
*  there are no events to react to the program sleeps and consumes very
*  little CPU time.  See main() to see how the event sources and event queue
*  are set up.
*/
static void main_loop(ALLEGRO_DISPLAY * display, ALLEGRO_EVENT_QUEUE *queue, ALLEGRO_THREAD * main_thread, DFHack::color_ostream & con)
{
    ALLEGRO_EVENT event;
    while (!al_get_thread_should_stop(main_thread)) {
        if (redraw && al_event_queue_is_empty(queue)) {
            al_rest(0);
            if(ssConfig.spriteIndexOverlay) {
                DrawSpriteIndexOverlay(ssConfig.currentSpriteOverlay);
            } else if(!Maps::IsValid()) {
                drawcredits();
            } else if( timeToReloadSegment ) {
                reloadDisplayedSegment();
                al_clear_to_color(ssConfig.backcol);
                paintboard();
                timeToReloadSegment = false;
                animationFrameShown = true;
            } else if (animationFrameShown == false) {
                al_clear_to_color(ssConfig.backcol);
                paintboard();
                animationFrameShown = true;
            }
            doMouse();
 			doRepeatActions();
            redraw = false;
        }
        /* Take the next event out of the event queue, and store it in `event'. */
        bool in_time = 0;
        in_time = al_wait_for_event_timed(queue, &event, 1.0f);

        /* Check what type of event we got and act accordingly.  ALLEGRO_EVENT
        * is a union type and interpretation of its contents is dependent on
        * the event type, which is given by the 'type' field.
        *
        * Each event also comes from an event source and has a timestamp.
        * These are accessible through the 'any.source' and 'any.timestamp'
        * fields respectively, e.g. 'event.any.timestamp'
        */

        if(in_time) {
            switch (event.type) {
            case ALLEGRO_EVENT_DISPLAY_RESIZE:
                if(!al_acknowledge_resize(event.display.source)) {
                    con.printerr("Failed to resize diplay");
                    return;
                }
                timeToReloadSegment = true;
                redraw = true;
#if 1
                {
                    //XXX the opengl drivers currently don't resize the backbuffer
                    ALLEGRO_BITMAP *bb = al_get_backbuffer(al_get_current_display());
                    int w = al_get_bitmap_width(bb);
                    int h = al_get_bitmap_height(bb);
                    ssState.ScreenH = h;
                    ssState.ScreenW = w;
                    PrintMessage("backbuffer w, h: %d, %d\n", w, h);
                }
#endif
                /* ALLEGRO_EVENT_KEY_DOWN - a keyboard key was pressed.
                */
            case ALLEGRO_EVENT_KEY_CHAR:
                if(event.keyboard.display != display) {
                    break;
                } else if (event.keyboard.keycode == ALLEGRO_KEY_ESCAPE) {
                    return;
                } else {
                    doKeys(event.keyboard.keycode, event.keyboard.modifiers);
                    redraw = true;
                }
                break;

                /* ALLEGRO_EVENT_DISPLAY_CLOSE - the window close button was pressed.
                */
            case ALLEGRO_EVENT_DISPLAY_CLOSE:
                return;

            case ALLEGRO_EVENT_TIMER:
                if(event.timer.source == reloadtimer) {
                    timeToReloadSegment = true;
                    redraw = true;
                } else if (event.timer.source == animationtimer) {
                    animUpdateProc();
                    redraw = true;
                }
                /* We received an event of some type we don't know about.
                * Just ignore it.
                */
            default:
                break;
            }
        }
    }
}

//replacement for main()
static void * stonesense_thread(ALLEGRO_THREAD * main_thread, void * parms)
{
    color_ostream_proxy out(Core::getInstance().getConsole());
    out.print("Stonesense launched\n");

    ssConfig.debug_mode = false;
    ssConfig.hide_outer_tiles = false;
    ssConfig.shade_hidden_tiles = true;
    ssConfig.load_ground_materials = true;
    ssConfig.automatic_reload_time = 0;
    ssConfig.automatic_reload_step = 500;
    ssConfig.lift_segment_offscreen_x = 0;
    ssConfig.lift_segment_offscreen_y = 0;
    ssConfig.Fullscreen = FULLSCREEN;
    ssState.ScreenH = RESOLUTION_HEIGHT;
    ssState.ScreenW = RESOLUTION_WIDTH;
    ssState.SegmentSize.x = DEFAULT_SEGMENTSIZE;
    ssState.SegmentSize.y = DEFAULT_SEGMENTSIZE;
    ssState.SegmentSize.z = DEFAULT_SEGMENTSIZE_Z;
    ssConfig.show_creature_names = true;
    ssConfig.show_osd = true;
    ssConfig.show_keybinds = false;
    ssConfig.show_intro = true;
    ssConfig.track_center = false;
    ssConfig.track_screen_center = true;
    ssConfig.animation_step = 300;
    ssConfig.follow_DFscreen = false;
    timeToReloadConfig = true;
    ssConfig.fogcol = al_map_rgba(255, 255, 255, 255);
    ssConfig.backcol = al_map_rgb(95, 95, 160);
    ssConfig.fogenable = true;
    ssConfig.bitmapHolds = 4096;
    ssConfig.imageCacheSize = 4096;
    ssConfig.fontsize = 10;
    ssConfig.font = al_create_path("data/art/font.ttf");
    ssConfig.creditScreen = true;
    ssConfig.bloodcutoff = 100;
    ssConfig.poolcutoff = 100;
    ssConfig.threadmade = 0;
    ssConfig.threading_enable = 1;
    ssConfig.fog_of_war = 1;
    ssConfig.occlusion = 1;
    contentLoader = new ContentLoader();
    ssConfig.zoom = 0;
    ssConfig.scale = 1.0f;
    ssTimers.assembly_time = 1.0f;
    ssTimers.beautify_time = 1.0f;
    ssTimers.draw_time = 1.0f;
    ssTimers.read_time = 1.0f;
    ssTimers.prev_frame_time = clock();
    ssTimers.frame_total = 1.0f;
    initRandomCube();
    loadConfigFile();
    loadKeymapFile();
    init_masks();
    if(!loadfont(out)) {
        stonesense_started = 0;
        return NULL;
    }
    //set debug cursor
    debugCursor.x = ssState.SegmentSize.x / 2;
    debugCursor.y = ssState.SegmentSize.y / 2;

    uint32_t version = al_get_allegro_version();
    int major = version >> 24;
    int minor = (version >> 16) & 255;
    int revision = (version >> 8) & 255;
    int release = version & 255;

    out.print("Using allegro version %d.%d.%d r%d\n", major, minor, revision, release);

    int gfxMode = ssConfig.Fullscreen ? ALLEGRO_FULLSCREEN : ALLEGRO_WINDOWED;
    al_set_new_display_flags(gfxMode|ALLEGRO_RESIZABLE|(ssConfig.opengl ? ALLEGRO_OPENGL : 0)|(ssConfig.directX ? ALLEGRO_DIRECT3D_INTERNAL : 0));
    display = al_create_display(ssState.ScreenW, ssState.ScreenH);
    if (!display) {
        out.printerr("al_create_display failed\n");
        stonesense_started = 0;
        return NULL;
    }
    if(!al_is_keyboard_installed()) {
        if (!al_install_keyboard()) {
            out.printerr("Stonesense: al_install_keyboard failed\n");
        }
    }
    if(!al_is_mouse_installed()) {
        if (!al_install_mouse()) {
            out.printerr("Stonesense: al_install_mouse failed\n");
        }
    }
    SetTitle("Stonesense");

    if(ssConfig.software) {
        al_set_new_bitmap_flags(ALLEGRO_MEMORY_BITMAP|ALLEGRO_ALPHA_TEST|ALLEGRO_MIN_LINEAR|ALLEGRO_MIPMAP);
    } else {
        al_set_new_bitmap_flags(ALLEGRO_MIN_LINEAR|ALLEGRO_MIPMAP);
    }

    ALLEGRO_PATH * p = al_create_path("stonesense/stonesense.png");
    IMGIcon = load_bitmap_withWarning(al_path_cstr(p, ALLEGRO_NATIVE_PATH_SEP));
    al_destroy_path(p);
    if(!IMGIcon) {
        al_destroy_display(display);
        stonesense_started = 0;
        return NULL;
    }

    al_set_display_icon(display, IMGIcon);

    ALLEGRO_EVENT_QUEUE *queue;
    queue = al_create_event_queue();
    if (!queue) {
        out.printerr("al_create_event_queue failed\n");
        stonesense_started = 0;
        return NULL;
    }

    loadGraphicsFromDisk();
    al_clear_to_color(al_map_rgb(0,0,0));
    al_draw_textf(font, al_map_rgb(255,255,255), ssState.ScreenW/2, ssState.ScreenH/2, ALLEGRO_ALIGN_CENTRE, "Starting up...");
    al_flip_display();

    reloadtimer = al_create_timer(ALLEGRO_MSECS_TO_SECS(ssConfig.automatic_reload_time));
    animationtimer = al_create_timer(ALLEGRO_MSECS_TO_SECS(ssConfig.animation_step));

    if(ssConfig.animation_step) {
        al_start_timer(animationtimer);
    }


    al_register_event_source(queue, al_get_keyboard_event_source());
    al_register_event_source(queue, al_get_display_event_source(display));
    al_register_event_source(queue, al_get_mouse_event_source());
    al_register_event_source(queue, al_get_timer_event_source(reloadtimer));
    al_register_event_source(queue, al_get_timer_event_source(animationtimer));

    ssConfig.readCond = al_create_cond();

#ifdef BENCHMARK
    benchmark();
#endif
    // init map segment wrapper and its lock, start the reload thread.
    initAutoReload();

    timeToReloadSegment = false;
    // enter event loop here:
    main_loop(display, queue, main_thread, out);

    // window is destroyed.
    al_destroy_display(display);

    if(ssConfig.threadmade) {
        al_broadcast_cond(ssConfig.readCond);
        al_destroy_thread(ssConfig.readThread);
        ssConfig.spriteIndexOverlay = 0;
    }
    flushImgFiles();

    // remove the uranium fuel from the reactor... or map segment from the clutches of other threads.
    map_segment.lock();
    map_segment.shutdown();
    map_segment.unlock();
    al_destroy_bitmap(IMGIcon);
    IMGIcon = 0;
    delete contentLoader;
    contentLoader = 0;
    out.print("Stonesense shutdown.\n");
    stonesense_started = 0;
    return NULL;
}

//All this fun DFhack stuff I gotta do now.
DFhackCExport command_result stonesense_command(color_ostream &out, std::vector<std::string> & params);

//set the plugin name/dfhack version
DFHACK_PLUGIN("stonesense");

//This is the init command. it includes input options.
DFhackCExport command_result plugin_init ( color_ostream &out, std::vector <PluginCommand> &commands)
{
    commands.push_back(PluginCommand("stonesense","Start up the stonesense visualiser.",stonesense_command));
    commands.push_back(PluginCommand("ssense","Start up the stonesense visualiser.",stonesense_command));
    return CR_OK;
}

//this command is called every frame DF.
DFhackCExport command_result plugin_onupdate ( color_ostream &out )
{
    return CR_OK;
}

//And the shutdown command.
DFhackCExport command_result plugin_shutdown ( color_ostream &out )
{
    if(stonesense_event_thread) {
        al_join_thread(stonesense_event_thread, NULL);
    }
    al_uninstall_system();
    return CR_OK;
}

//and the actual stonesense command. Maybe.
DFhackCExport command_result stonesense_command(color_ostream &out, std::vector<std::string> & params)
{
    if(stonesense_started) {
        out.print("Stonesense already running.\n");
        return CR_OK;
    }

    if(params.size() > 0 ) {
        DumpInfo(out, params);
        return CR_OK;
    }

    stonesense_started = true;
    if(!al_is_system_installed()) {
        if (!al_init()) {
            out.printerr("Could not init Allegro.\n");
            return CR_FAILURE;
        }

        if (!al_init_image_addon()) {
            out.printerr("al_init_image_addon failed. \n");
            return CR_FAILURE;
        }
        if (!al_init_primitives_addon()) {
            out.printerr("al_init_primitives_addon failed. \n");
            return CR_FAILURE;
        }
        al_init_font_addon();
        if (!al_init_ttf_addon()) {
            out.printerr("al_init_ttf_addon failed. \n");
            return CR_FAILURE;
        }
    }

    stonesense_event_thread = al_create_thread(stonesense_thread, (void*) &out);
    al_start_thread(stonesense_event_thread);
    return CR_OK;
}
