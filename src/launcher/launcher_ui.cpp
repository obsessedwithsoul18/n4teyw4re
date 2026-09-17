/*
 * launcher_ui.cpp
 *
 * Runs as the first thing n4teyw4re.exe does when launched without
 * --n4teyw4re-ready.  Shows the mode-selection GUI, maps the driver, then
 * re-launches self with  --n4teyw4re-ready --km  or  --n4teyw4re-ready --um
 * and returns so the caller can exit.
 *
 * All rendering is done on a private D3D11 device / HWND so it has zero
 * coupling to the main overlay renderer.
 */

#include <stdafx.hpp>
#include <launcher/launcher_ui.hpp>
#include <launcher/driver_mapper.hpp>

#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shellapi.h>

// Must be declared at global scope so the linker resolves it to the symbol
// exported by imgui_impl_win32.cpp — not namespaced, not static.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND, UINT, WPARAM, LPARAM );

// ============================================================================
//  Anonymous namespace — everything is private to this translation unit
// ============================================================================
namespace
{

// ----------------------------------------------------------------------------
//  Palette
// ----------------------------------------------------------------------------
namespace pal
{
    constexpr ImVec4 bg       {  10/255.f,   9/255.f,  18/255.f, 1.f };
    constexpr ImVec4 card     {  20/255.f,  18/255.f,  34/255.f, 1.f };
    constexpr ImVec4 card_sel {  28/255.f,  16/255.f,  52/255.f, 1.f };
    constexpr ImVec4 sidebar  {  13/255.f,  11/255.f,  22/255.f, 1.f };
    constexpr ImVec4 purple      { 140/255.f,  60/255.f, 255/255.f, 1.f   };
    constexpr ImVec4 purple_dim  {  90/255.f,  35/255.f, 180/255.f, 1.f   };
    constexpr ImVec4 text_hi     { 240/255.f, 238/255.f, 255/255.f, 1.f   };
    constexpr ImVec4 text_mid    { 170/255.f, 165/255.f, 200/255.f, 1.f   };
    constexpr ImVec4 text_lo     {  90/255.f,  85/255.f, 120/255.f, 1.f   };
    constexpr ImVec4 green       {  72/255.f, 199/255.f, 116/255.f, 1.f   };
    constexpr ImVec4 red         { 235/255.f,  87/255.f,  87/255.f, 1.f   };
    constexpr ImVec4 border      {   1.f,       1.f,       1.f,     0.07f };
    constexpr ImVec4 border_sel  { 140/255.f,  60/255.f, 255/255.f, 0.70f };
}

static ImU32 col( const ImVec4& c )
{
    return ImGui::ColorConvertFloat4ToU32( c );
}
static ImU32 col_a( const ImVec4& c, float a )
{
    ImVec4 t = c; t.w = a;
    return ImGui::ColorConvertFloat4ToU32( t );
}

// ----------------------------------------------------------------------------
//  D3D11 state — all prefixed lc_ (launcher context) to avoid ODR clashes
// ----------------------------------------------------------------------------
static ID3D11Device*           lc_dev  = nullptr;
static ID3D11DeviceContext*    lc_ctx  = nullptr;
static IDXGISwapChain*         lc_sc   = nullptr;
static ID3D11RenderTargetView* lc_rtv  = nullptr;
static HWND                    lc_hwnd = nullptr;

static constexpr int LC_W = 900, LC_H = 580;

static bool lc_create_device( HWND hwnd )
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate             = { 60, 1 };
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL fl[]  = { D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL       fl_out{};
    if ( FAILED( D3D11CreateDeviceAndSwapChain( nullptr, D3D_DRIVER_TYPE_HARDWARE,
             nullptr, 0, fl, 1, D3D11_SDK_VERSION,
             &sd, &lc_sc, &lc_dev, &fl_out, &lc_ctx ) ) )
        return false;

    ID3D11Texture2D* bb = nullptr;
    lc_sc->GetBuffer( 0, IID_PPV_ARGS( &bb ) );
    if ( bb ) { lc_dev->CreateRenderTargetView( bb, nullptr, &lc_rtv ); bb->Release(); }
    return lc_rtv != nullptr;
}

static void lc_cleanup_device()
{
    if ( lc_rtv ) { lc_rtv->Release(); lc_rtv = nullptr; }
    if ( lc_sc  ) { lc_sc ->Release(); lc_sc  = nullptr; }
    if ( lc_ctx ) { lc_ctx ->Release(); lc_ctx = nullptr; }
    if ( lc_dev ) { lc_dev ->Release(); lc_dev = nullptr; }
}

static void lc_rebuild_rtv( UINT w, UINT h )
{
    if ( lc_rtv ) { lc_rtv->Release(); lc_rtv = nullptr; }
    lc_sc->ResizeBuffers( 0, w, h, DXGI_FORMAT_UNKNOWN, 0 );
    ID3D11Texture2D* bb = nullptr;
    lc_sc->GetBuffer( 0, IID_PPV_ARGS( &bb ) );
    if ( bb ) { lc_dev->CreateRenderTargetView( bb, nullptr, &lc_rtv ); bb->Release(); }
}

// ----------------------------------------------------------------------------
//  UI state
// ----------------------------------------------------------------------------
enum class Step { Mode, Inject, Done };

static Step              lc_step       = Step::Mode;
static bool              lc_km_mode    = false;          // false = UM default
static bool              lc_running    = true;
static bool              lc_dragging   = false;
static POINT             lc_drag_off   = {};
static float             lc_spin       = 0.f;
static std::atomic<bool> lc_done       { false };
static bool              lc_ok         = false;
static std::string       lc_msg;
static std::vector<std::string> lc_log;

static void push_log( std::string s )
{
    lc_log.push_back( std::move( s ) );
}

// Background thread: maps the driver in-process, updates lc_ok / lc_msg / lc_done.
static void inject_thread( bool km )
{
    push_log( "[ >> ] Loading embedded driver..." );
    ::Sleep( 150 );

    push_log( std::string( "[ >> ] Mapping " ) +
              ( km ? "N4TEYW4RE.Driver.sys" : "km.sys" ) +
              " via in-process kdmapper..." );

    const std::string err = launcher::map_driver( km );

    if ( !err.empty() )
    {
        push_log( "[ !! ] " + err );
        lc_msg = err;
        lc_ok  = false;
        lc_done = true;
        return;
    }

    push_log( "[ OK ] Driver mapped successfully." );
    push_log( "[ >> ] Relaunching with mode flag..." );

    const std::wstring flag = km ? L"--n4teyw4re-ready --km"
                                 : L"--n4teyw4re-ready --um";
    if ( !launcher::relaunch_self( flag ) )
    {
        push_log( "[ !! ] Failed to relaunch executable." );
        lc_msg = "Failed to relaunch the executable after mapping.";
        lc_ok  = false;
        lc_done = true;
        return;
    }

    push_log( "[ OK ] " + std::string( km ? "n4teyw4re" : "um.exe" ) + " is starting up." );
    lc_msg  = km ? "n4teyw4re launched successfully."
                 : "UM client launched successfully.";
    lc_ok   = true;
    lc_done = true;
}

// ----------------------------------------------------------------------------
//  Draw helpers (all identical to standalone launcher)
// ----------------------------------------------------------------------------
static void spaced_text( const char* txt, float sp, const ImVec4& color )
{
    ImGui::PushStyleColor( ImGuiCol_Text, color );
    for ( const char* p = txt; *p; ++p )
    {
        char b[2] = { *p, 0 };
        ImGui::TextUnformatted( b );
        ImGui::SameLine( 0.f, sp );
    }
    ImGui::NewLine();
    ImGui::PopStyleColor();
}

static void draw_grid( ImDrawList* dl, ImVec2 p0, ImVec2 p1 )
{
    static const float pts[][2] = {
        {0.13f,0.12f},{0.38f,0.05f},{0.63f,0.05f},{0.87f,0.12f},
        {0.05f,0.50f},{0.95f,0.50f},
        {0.13f,0.88f},{0.38f,0.95f},{0.63f,0.95f},{0.87f,0.88f},
        {0.50f,0.50f},
    };
    const float W = p1.x - p0.x, H = p1.y - p0.y;
    const ImU32 c = col_a( pal::purple, 0.20f );
    for ( auto& pt : pts )
    {
        const float x = p0.x + pt[0] * W, y = p0.y + pt[1] * H;
        constexpr float s = 5.f;
        dl->AddLine( ImVec2{x-s,y}, ImVec2{x+s,y}, c, 1.f );
        dl->AddLine( ImVec2{x,y-s}, ImVec2{x,y+s}, c, 1.f );
    }
}

static void draw_icon_monitor( ImDrawList* dl, ImVec2 c, float sz, ImU32 clr )
{
    const float hw = sz*.55f, hh = sz*.40f, r = sz*.08f;
    dl->AddRect( ImVec2{c.x-hw,c.y-hh}, ImVec2{c.x+hw,c.y+hh*.55f}, clr, r, 0, 1.8f );
    dl->AddLine( ImVec2{c.x,c.y+hh*.55f}, ImVec2{c.x,c.y+hh}, clr, 1.6f );
    dl->AddLine( ImVec2{c.x-hw*.4f,c.y+hh}, ImVec2{c.x+hw*.4f,c.y+hh}, clr, 1.6f );
}

static void draw_icon_chip( ImDrawList* dl, ImVec2 c, float sz, ImU32 clr )
{
    const float h = sz*.42f;
    dl->AddRect( ImVec2{c.x-h,c.y-h}, ImVec2{c.x+h,c.y+h}, clr, sz*.07f, 0, 1.8f );
    dl->AddRect( ImVec2{c.x-h*.5f,c.y-h*.5f}, ImVec2{c.x+h*.5f,c.y+h*.5f}, clr, 2.f, 0, 1.2f );
    const float pg = h*.35f, pl = h*.30f;
    for ( int i = -1; i <= 1; ++i )
    {
        const float o = i * pg;
        dl->AddLine( ImVec2{c.x+o,c.y-h},  ImVec2{c.x+o,c.y-h-pl}, clr, 1.5f );
        dl->AddLine( ImVec2{c.x+o,c.y+h},  ImVec2{c.x+o,c.y+h+pl}, clr, 1.5f );
        dl->AddLine( ImVec2{c.x-h,c.y+o},  ImVec2{c.x-h-pl,c.y+o}, clr, 1.5f );
        dl->AddLine( ImVec2{c.x+h,c.y+o},  ImVec2{c.x+h+pl,c.y+o}, clr, 1.5f );
    }
}

static void draw_check_badge( ImDrawList* dl, ImVec2 pos, float r, bool filled )
{
    if ( filled )
    {
        dl->AddCircleFilled( pos, r, col( pal::purple ) );
        ImVec2 tick[3] = {
            ImVec2{pos.x-r*.45f, pos.y},
            ImVec2{pos.x-r*.10f, pos.y+r*.42f},
            ImVec2{pos.x+r*.50f, pos.y-r*.40f}
        };
        dl->AddPolyline( tick, 3, IM_COL32_WHITE, 0, 1.6f );
    }
    else
    {
        dl->AddCircle( pos, r, col_a( pal::purple, 0.35f ), 0, 1.3f );
    }
}

static void draw_spinner( ImDrawList* dl, ImVec2 c, float r, float t )
{
    constexpr int   N   = 48;
    constexpr float ARC = 1.6f;
    for ( int i = 0; i < N; ++i )
    {
        const float a0 = t + ARC * (float)i / N;
        const float a1 = t + ARC * (float)(i+1) / N;
        dl->AddLine(
            ImVec2{c.x+cosf(a0)*r, c.y+sinf(a0)*r},
            ImVec2{c.x+cosf(a1)*r, c.y+sinf(a1)*r},
            col_a( pal::purple, 0.12f + 0.88f*(float)i/N ), 2.8f );
    }
}

// ----------------------------------------------------------------------------
//  Sidebar
// ----------------------------------------------------------------------------
static void draw_sidebar( ImVec2 origin, float w, float h )
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled( origin, ImVec2{origin.x+w,origin.y+h}, col(pal::sidebar) );
    dl->AddLine( ImVec2{origin.x+w,origin.y}, ImVec2{origin.x+w,origin.y+h},
                 col_a(pal::purple,0.15f), 1.f );

    ImGui::SetCursorScreenPos( ImVec2{origin.x+18.f,origin.y+22.f} );
    ImGui::PushStyleColor( ImGuiCol_Text, pal::text_hi );
    ImGui::SetWindowFontScale( 1.15f );
    ImGui::TextUnformatted( "N4TEY" );
    ImGui::SetWindowFontScale( 1.f );
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos( ImVec2{origin.x+18.f,origin.y+42.f} );
    ImGui::PushStyleColor( ImGuiCol_Text, pal::text_lo );
    ImGui::TextUnformatted( "CS2 EXTERNAL LOADER" );
    ImGui::PopStyleColor();

    struct SideStep { const char* title; const char* sub; Step step; };
    const SideStep steps[4] = {
        { "MODE",   "Select execution method", Step::Mode   },
        { "CONFIG", "Load or create config.",  Step::Mode   },
        { "INJECT", "Start execution ...",     Step::Inject },
        { "DONE",   "You're ready.",           Step::Done   },
    };

    float sy = origin.y + 110.f;
    for ( int i = 0; i < 4; ++i )
    {
        const bool active =
            ( i == 0 && lc_step == Step::Mode   ) ||
            ( i == 2 && lc_step == Step::Inject ) ||
            ( i == 3 && lc_step == Step::Done   );
        const bool reached =
            ( i == 0 ) ||
            ( i == 2 && lc_step >= Step::Inject ) ||
            ( i == 3 && lc_step == Step::Done  );

        const ImVec2 dot = ImVec2{origin.x+22.f, sy+7.f};
        if ( active )
        {
            dl->AddCircleFilled( dot, 5.f, col(pal::purple) );
            dl->AddRectFilled( ImVec2{origin.x,sy-2.f},
                               ImVec2{origin.x+3.f,sy+22.f}, col(pal::purple) );
        }
        else
        {
            dl->AddCircle( dot, 4.f,
                reached ? col_a(pal::purple,0.55f) : col_a(pal::purple,0.22f),
                0, 1.2f );
        }

        ImGui::SetCursorScreenPos( ImVec2{origin.x+34.f, sy-1.f} );
        ImGui::PushStyleColor( ImGuiCol_Text, active ? pal::text_hi : pal::text_lo );
        ImGui::TextUnformatted( steps[i].title );
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos( ImVec2{origin.x+34.f, sy+14.f} );
        ImGui::PushStyleColor( ImGuiCol_Text, pal::text_lo );
        ImGui::TextUnformatted( steps[i].sub );
        ImGui::PopStyleColor();

        sy += 58.f;
    }

    ImGui::SetCursorScreenPos( ImVec2{origin.x+16.f, origin.y+h-44.f} );
    ImGui::PushStyleColor( ImGuiCol_Text, pal::text_lo );
    ImGui::TextUnformatted( "VERSION 1.0.0" );
    ImGui::SetCursorScreenPos( ImVec2{origin.x+16.f, origin.y+h-28.f} );
    ImGui::TextUnformatted( "PRIVATE BUILD" );
    ImGui::PopStyleColor();
}

// ----------------------------------------------------------------------------
//  MODE page
// ----------------------------------------------------------------------------
static void draw_mode_page( ImVec2 o, float w, float h )
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // top label
    {
        const char* t = "COUNTER STRIKE 2";
        const ImVec2 sz = ImGui::CalcTextSize( t );
        ImGui::SetCursorScreenPos( ImVec2{o.x+(w-sz.x)*.5f, o.y+28.f} );
        ImGui::PushStyleColor( ImGuiCol_Text, pal::text_lo );
        ImGui::TextUnformatted( t );
        ImGui::PopStyleColor();
    }

    // big spaced title
    {
        constexpr float sp = 4.5f;
        const char* t = "SELECT EXECUTION MODE";
        float tw = 0.f;
        for ( const char* p = t; *p; ++p ) { char b[2]={*p,0}; tw += ImGui::CalcTextSize(b).x+sp; }
        ImGui::SetCursorScreenPos( ImVec2{o.x+(w-tw)*.5f, o.y+50.f} );
        ImGui::SetWindowFontScale( 1.38f );
        spaced_text( t, sp, pal::text_hi );
        ImGui::SetWindowFontScale( 1.f );
    }

    // subtitle
    {
        const char* s = "Choose how you want to run the external.";
        const ImVec2 sz = ImGui::CalcTextSize( s );
        ImGui::SetCursorScreenPos( ImVec2{o.x+(w-sz.x)*.5f, o.y+92.f} );
        ImGui::PushStyleColor( ImGuiCol_Text, pal::text_mid );
        ImGui::TextUnformatted( s );
        ImGui::PopStyleColor();
    }

    // top-right tagline
    {
        const float lx = o.x+w-130.f;
        ImGui::SetCursorScreenPos( ImVec2{lx, o.y+24.f} );
        ImGui::PushStyleColor( ImGuiCol_Text, pal::text_lo );
        ImGui::TextUnformatted( "BUILT DIFFERENT" );
        ImGui::SetCursorScreenPos( ImVec2{lx, o.y+38.f} );
        ImGui::TextUnformatted( "FOR A HIGHER EDGE" );
        ImGui::PopStyleColor();
        dl->AddLine( ImVec2{lx,o.y+52.f}, ImVec2{lx+60.f,o.y+52.f}, col(pal::purple), 1.f );
    }

    // cards
    constexpr float CW = 240.f, CH = 260.f;
    const float cy0    = o.y + 118.f;
    const float cx_um  = o.x + (w - (CW*2.f+24.f))*.5f;
    const float cx_km  = cx_um + CW + 24.f;

    auto draw_card = [&]( float cx, bool is_um ) -> bool
    {
        const bool sel = is_um ? !lc_km_mode : lc_km_mode;
        const ImVec2 p0 = ImVec2{cx,cy0};
        const ImVec2 p1 = ImVec2{cx+CW,cy0+CH};
        const bool hov  = ImGui::IsMouseHoveringRect(p0,p1);
        const bool clk  = hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

        if ( sel )
            for ( int g=8; g>=1; --g )
            {
                const float e = (float)g*2.5f;
                dl->AddRectFilled(
                    ImVec2{p0.x-e,p0.y-e}, ImVec2{p1.x+e,p1.y+e},
                    col_a(pal::purple, 0.025f*(9-g)), 16.f );
            }

        dl->AddRectFilled( p0, p1,
            sel ? col(pal::card_sel) : (hov ? col_a(pal::card_sel,0.5f) : col(pal::card)),
            12.f );
        dl->AddRect( p0, p1,
            sel ? col(pal::border_sel) : (hov ? col_a(pal::border_sel,0.35f) : col(pal::border)),
            12.f, 0, 1.4f );

        const ImVec2 ic = ImVec2{cx+CW*.5f, cy0+68.f};
        const ImU32  ic32 = sel ? col(pal::purple) : col_a(pal::purple,0.55f);
        if ( is_um ) draw_icon_monitor( dl, ic, 42.f, ic32 );
        else         draw_icon_chip   ( dl, ic, 42.f, ic32 );

        draw_check_badge( dl, ImVec2{p1.x-22.f,p0.y+22.f}, 11.f, sel );

        // title
        const char* title = is_um ? "USERMODE" : "KERNEL MODE";
        constexpr float tsp = 3.5f;
        float tw = 0.f;
        for ( const char* p=title; *p; ++p ) { char b[2]={*p,0}; tw+=ImGui::CalcTextSize(b).x+tsp; }
        ImGui::SetCursorScreenPos( ImVec2{cx+(CW-tw)*.5f, cy0+118.f} );
        ImGui::SetWindowFontScale( 1.05f );
        spaced_text( title, tsp, sel ? pal::text_hi : pal::text_mid );
        ImGui::SetWindowFontScale( 1.f );

        // tagline
        const char* tag = is_um ? "FAST. STEALTHY. STABLE." : "DEEP ACCESS. MAXIMUM CONTROL.";
        const ImVec2 tgsz = ImGui::CalcTextSize( tag );
        ImGui::SetCursorScreenPos( ImVec2{cx+(CW-tgsz.x)*.5f, cy0+142.f} );
        ImGui::PushStyleColor( ImGuiCol_Text, pal::text_lo );
        ImGui::TextUnformatted( tag );
        ImGui::PopStyleColor();

        dl->AddLine( ImVec2{p0.x+16.f,cy0+162.f}, ImVec2{p1.x-16.f,cy0+162.f},
                     col_a(pal::purple,0.18f), 1.f );

        // bullets
        const char* fu[] = { "Lower detection risk","Faster injection","Recommended for most users" };
        const char* fk[] = { "Higher privilege access","Advanced functionality","Use only if you know what you're doing" };
        const char** feats = is_um ? fu : fk;
        for ( int i = 0; i < 3; ++i )
        {
            const float fy = cy0+176.f+i*24.f;
            dl->AddCircleFilled( ImVec2{cx+22.f,fy+7.f}, 3.5f,
                sel ? col(pal::purple) : col_a(pal::purple,0.45f) );
            ImGui::SetCursorScreenPos( ImVec2{cx+34.f,fy} );
            ImGui::PushStyleColor( ImGuiCol_Text, sel ? pal::text_mid : pal::text_lo );
            ImGui::TextUnformatted( feats[i] );
            ImGui::PopStyleColor();
        }

        ImGui::SetCursorScreenPos( p0 );
        ImGui::InvisibleButton( is_um ? "##um" : "##km", ImVec2{CW,CH} );
        return clk;
    };

    if ( draw_card( cx_um, true  ) ) lc_km_mode = false;
    if ( draw_card( cx_km, false ) ) lc_km_mode = true;

    // CONTINUE button
    constexpr float BW = 220.f, BH = 46.f;
    const float bx = o.x+(w-BW)*.5f, by = o.y+h-80.f;
    const ImVec2 bp0 = ImVec2{bx,by}, bp1 = ImVec2{bx+BW,by+BH};
    const bool bhov = ImGui::IsMouseHoveringRect(bp0,bp1);
    const bool bclk = bhov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    if ( bhov )
        dl->AddRectFilled( ImVec2{bp0.x-4.f,bp0.y-4.f}, ImVec2{bp1.x+4.f,bp1.y+4.f},
                           col_a(pal::purple,0.12f), 28.f );
    dl->AddRectFilled( bp0, bp1,
        bhov ? col(pal::purple_dim) : col_a(pal::purple_dim,0.55f), 24.f );
    dl->AddRect( bp0, bp1,
        bhov ? col(pal::purple) : col_a(pal::purple,0.55f), 24.f, 0, 1.4f );

    {
        constexpr float sp = 3.5f;
        float tw = 0.f;
        for ( const char* p="CONTINUE"; *p; ++p ) { char b[2]={*p,0}; tw+=ImGui::CalcTextSize(b).x+sp; }
        tw += 20.f;
        ImGui::SetCursorScreenPos( ImVec2{bp0.x+(BW-tw)*.5f,
                                          bp0.y+(BH-ImGui::GetTextLineHeight())*.5f} );
        spaced_text( "CONTINUE", sp, bhov ? pal::text_hi : pal::text_mid );
        dl->AddLine( ImVec2{bp0.x+(BW+tw)*.5f-12.f, by+BH*.5f},
                     ImVec2{bp0.x+(BW+tw)*.5f,       by+BH*.5f},
                     bhov ? col(pal::purple) : col_a(pal::purple,0.6f), 1.5f );
    }

    ImGui::SetCursorScreenPos( bp0 );
    ImGui::InvisibleButton( "##cont", ImVec2{BW,BH} );

    // bottom tagline
    const float ftx = o.x+w-140.f;
    ImGui::SetCursorScreenPos( ImVec2{ftx, o.y+h-40.f} );
    ImGui::PushStyleColor( ImGuiCol_Text, pal::text_lo );
    ImGui::TextUnformatted( "SAME GAME." );
    ImGui::SetCursorScreenPos( ImVec2{ftx, o.y+h-24.f} );
    ImGui::TextUnformatted( "DIFFERENT OUTCOMES." );
    ImGui::PopStyleColor();
    dl->AddLine( ImVec2{ftx,o.y+h-10.f}, ImVec2{ftx+60.f,o.y+h-10.f},
                 col(pal::purple), 1.f );

    if ( bclk )
    {
        lc_step = Step::Inject;
        lc_spin = 0.f;
        lc_log.clear();
        lc_done = false;
        std::thread( inject_thread, lc_km_mode ).detach();
    }
}

// ----------------------------------------------------------------------------
//  INJECT page
// ----------------------------------------------------------------------------
static void draw_inject_page( ImVec2 o, float w, float h )
{
    ImDrawList* dl = ImGui::GetWindowDrawList();

    {
        constexpr float sp = 4.f;
        const char* t = "INITIALISING";
        float tw = 0.f;
        for ( const char* p=t; *p; ++p ) { char b[2]={*p,0}; tw+=ImGui::CalcTextSize(b).x+sp; }
        ImGui::SetCursorScreenPos( ImVec2{o.x+(w-tw)*.5f, o.y+38.f} );
        ImGui::SetWindowFontScale( 1.3f );
        spaced_text( t, sp, pal::text_hi );
        ImGui::SetWindowFontScale( 1.f );
    }

    {
        const char* s = lc_km_mode
            ? "Mapping N4TEYW4RE.Driver.sys and relaunching..."
            : "Mapping km.sys and relaunching...";
        const ImVec2 sz = ImGui::CalcTextSize( s );
        ImGui::SetCursorScreenPos( ImVec2{o.x+(w-sz.x)*.5f, o.y+76.f} );
        ImGui::PushStyleColor( ImGuiCol_Text, pal::text_mid );
        ImGui::TextUnformatted( s );
        ImGui::PopStyleColor();
    }

    lc_spin += ImGui::GetIO().DeltaTime * 2.8f;
    draw_spinner( dl, ImVec2{o.x+w*.5f, o.y+160.f}, 28.f, lc_spin );

    // log box
    const ImVec2 lb0 = ImVec2{o.x+40.f, o.y+210.f};
    const ImVec2 lb1 = ImVec2{o.x+w-40.f, o.y+h-60.f};
    dl->AddRectFilled( lb0, lb1, col(pal::card), 6.f );
    dl->AddRect( lb0, lb1, col_a(pal::purple,0.20f), 6.f, 0, 1.f );

    const float lh = ImGui::GetTextLineHeightWithSpacing();
    const int   mx = (int)((lb1.y-lb0.y-20.f)/lh);
    const int   st = (int)lc_log.size()>mx ? (int)lc_log.size()-mx : 0;
    float ly = lb0.y+10.f;
    for ( int i = st; i < (int)lc_log.size(); ++i )
    {
        ImVec4 lc = pal::text_mid;
        if      ( lc_log[i].find("[ OK ]") != std::string::npos ) lc = pal::green;
        else if ( lc_log[i].find("[ !! ]") != std::string::npos ) lc = pal::red;
        else if ( lc_log[i].find("[ >> ]") != std::string::npos ) lc = pal::purple;
        ImGui::SetCursorScreenPos( ImVec2{lb0.x+12.f, ly} );
        ImGui::PushStyleColor( ImGuiCol_Text, lc );
        ImGui::TextUnformatted( lc_log[i].c_str() );
        ImGui::PopStyleColor();
        ly += lh;
    }

    if ( lc_done.load() ) lc_step = Step::Done;
}

// ----------------------------------------------------------------------------
//  DONE page
// ----------------------------------------------------------------------------
static void draw_done_page( ImVec2 o, float w, float h )
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cx = o.x+w*.5f, cy = o.y+h*.5f-50.f;

    if ( lc_ok )
    {
        dl->AddCircleFilled( ImVec2{cx,cy}, 40.f, col_a(pal::green,0.10f) );
        dl->AddCircle      ( ImVec2{cx,cy}, 40.f, col(pal::green), 0, 1.8f );
        ImVec2 tick[3] = {
            ImVec2{cx-14.f,cy}, ImVec2{cx-4.f,cy+11.f}, ImVec2{cx+16.f,cy-13.f}
        };
        dl->AddPolyline( tick, 3, col(pal::green), 0, 2.5f );
    }
    else
    {
        dl->AddCircleFilled( ImVec2{cx,cy}, 40.f, col_a(pal::red,0.10f) );
        dl->AddCircle      ( ImVec2{cx,cy}, 40.f, col(pal::red), 0, 1.8f );
        dl->AddLine( ImVec2{cx-13.f,cy-13.f}, ImVec2{cx+13.f,cy+13.f}, col(pal::red), 2.5f );
        dl->AddLine( ImVec2{cx+13.f,cy-13.f}, ImVec2{cx-13.f,cy+13.f}, col(pal::red), 2.5f );
    }

    {
        const char* s = lc_ok ? "SUCCESS" : "FAILED";
        float sw = 0.f;
        for ( const char* p=s; *p; ++p ) { char b[2]={*p,0}; sw+=ImGui::CalcTextSize(b).x+4.f; }
        ImGui::SetCursorScreenPos( ImVec2{cx-sw*.5f, cy+52.f} );
        ImGui::SetWindowFontScale( 1.2f );
        spaced_text( s, 4.f, lc_ok ? pal::green : pal::red );
        ImGui::SetWindowFontScale( 1.f );
    }

    {
        const ImVec2 msz = ImGui::CalcTextSize( lc_msg.c_str() );
        ImGui::SetCursorScreenPos( ImVec2{cx-msz.x*.5f, cy+82.f} );
        ImGui::PushStyleColor( ImGuiCol_Text, pal::text_mid );
        ImGui::TextUnformatted( lc_msg.c_str() );
        ImGui::PopStyleColor();
    }

    constexpr float BW = 160.f, BH = 40.f;
    const ImVec2 bp0 = ImVec2{cx-BW*.5f,cy+120.f}, bp1 = ImVec2{cx+BW*.5f,cy+120.f+BH};
    const bool bhov = ImGui::IsMouseHoveringRect(bp0,bp1);
    const bool bclk = bhov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    dl->AddRectFilled( bp0, bp1,
        bhov ? col(pal::purple_dim) : col_a(pal::purple_dim,0.4f), 20.f );
    dl->AddRect( bp0, bp1,
        bhov ? col(pal::purple) : col_a(pal::purple,0.4f), 20.f, 0, 1.2f );

    {
        const char* lbl = lc_ok ? "CLOSE" : "BACK";
        const ImVec2 lsz = ImGui::CalcTextSize( lbl );
        ImGui::SetCursorScreenPos( ImVec2{bp0.x+(BW-lsz.x)*.5f,
                                           bp0.y+(BH-lsz.y)*.5f} );
        ImGui::PushStyleColor( ImGuiCol_Text, bhov ? pal::text_hi : pal::text_mid );
        ImGui::TextUnformatted( lbl );
        ImGui::PopStyleColor();
    }

    ImGui::SetCursorScreenPos( bp0 );
    ImGui::InvisibleButton( "##done", ImVec2{BW,BH} );

    if ( bclk )
    {
        if ( lc_ok ) lc_running = false;   // close launcher window
        else         lc_step = Step::Mode; // try again
    }
}

// ----------------------------------------------------------------------------
//  Chrome buttons
// ----------------------------------------------------------------------------
static void draw_chrome( ImVec2 wmin, float win_w )
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float R = 10.f, GAP = 24.f;
    const float y  = wmin.y + 18.f;
    const float x3 = wmin.x + win_w - 22.f;
    const float x2 = x3 - GAP, x1 = x2 - GAP;

    auto chrome_btn = [&]( float cx, char sym ) -> bool
    {
        const ImVec2 p0 = ImVec2{cx-R,y-R}, p1 = ImVec2{cx+R,y+R};
        const bool hov = ImGui::IsMouseHoveringRect(p0,p1);
        const bool clk = hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if ( hov ) dl->AddCircleFilled( ImVec2{cx,y}, R, col_a(pal::purple,0.25f) );
        const ImU32 lc = hov ? col(pal::text_hi) : col(pal::text_lo);
        if      ( sym == '-' )
            dl->AddLine( ImVec2{cx-5.f,y},     ImVec2{cx+5.f,y},     lc, 1.4f );
        else if ( sym == 'o' )
            dl->AddRect( ImVec2{cx-5.f,y-4.f}, ImVec2{cx+5.f,y+4.f}, lc, 1.f, 0, 1.2f );
        else
        {
            dl->AddLine( ImVec2{cx-5.f,y-4.f}, ImVec2{cx+5.f,y+4.f}, lc, 1.4f );
            dl->AddLine( ImVec2{cx+5.f,y-4.f}, ImVec2{cx-5.f,y+4.f}, lc, 1.4f );
        }
        ImGui::SetCursorScreenPos( p0 );
        ImGui::InvisibleButton( &sym, ImVec2{R*2.f,R*2.f} );
        return clk;
    };

    if ( chrome_btn( x1, '-' ) ) ::ShowWindow( lc_hwnd, SW_MINIMIZE );
    chrome_btn( x2, 'o' );
    if ( chrome_btn( x3, 'x' ) ) lc_running = false;
}

// ----------------------------------------------------------------------------
//  Root frame
// ----------------------------------------------------------------------------
static void render_frame()
{
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos( ImVec2{0.f,0.f}, ImGuiCond_Always );
    ImGui::SetNextWindowSize( io.DisplaySize, ImGuiCond_Always );

    ImGui::PushStyleColor( ImGuiCol_WindowBg, pal::bg );
    ImGui::PushStyleColor( ImGuiCol_Border,   pal::border );
    ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding,   0.f );
    ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 0.f );
    ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding,    ImVec2{0.f,0.f} );

    ImGui::Begin( "##lc_root", nullptr,
        ImGuiWindowFlags_NoTitleBar    | ImGuiWindowFlags_NoResize  |
        ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar   | ImGuiWindowFlags_NoBringToFrontOnFocus );

    ImDrawList*  dl      = ImGui::GetWindowDrawList();
    const ImVec2 wmin    = ImGui::GetWindowPos();
    const float  W = io.DisplaySize.x, H = io.DisplaySize.y;

    dl->AddRect( wmin, ImVec2{wmin.x+W,wmin.y+H}, col_a(pal::purple,0.30f), 0.f, 0, 1.f );
    draw_grid( dl, wmin, ImVec2{wmin.x+W,wmin.y+H} );

    constexpr float SW = 180.f;
    draw_sidebar( wmin, SW, H );
    draw_chrome ( wmin, W  );

    const ImVec2 co = ImVec2{wmin.x+SW, wmin.y};
    switch ( lc_step )
    {
    case Step::Mode:   draw_mode_page  ( co, W-SW, H ); break;
    case Step::Inject: draw_inject_page( co, W-SW, H ); break;
    case Step::Done:   draw_done_page  ( co, W-SW, H ); break;
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ----------------------------------------------------------------------------
//  Window proc
// ----------------------------------------------------------------------------
static LRESULT CALLBACK lc_wnd_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp )
{
    if ( ImGui_ImplWin32_WndProcHandler( hwnd, msg, wp, lp ) ) return TRUE;
    switch ( msg )
    {
    case WM_LBUTTONDOWN:
    {
        POINT pt = { (LONG)LOWORD(lp), (LONG)HIWORD(lp) };
        if ( pt.y < 40 && pt.x < LC_W - 80 )
        {
            lc_dragging = true;
            lc_drag_off = pt;
            ::SetCapture( hwnd );
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if ( lc_dragging )
        {
            POINT cur{};
            ::GetCursorPos( &cur );
            ::SetWindowPos( hwnd, nullptr,
                cur.x - lc_drag_off.x, cur.y - lc_drag_off.y,
                0, 0, SWP_NOSIZE | SWP_NOZORDER );
        }
        return 0;
    case WM_LBUTTONUP:
        lc_dragging = false; ::ReleaseCapture(); return 0;
    case WM_DESTROY:
        lc_running = false; ::PostQuitMessage(0); return 0;
    case WM_SIZE:
        if ( lc_dev && wp != SIZE_MINIMIZED )
            lc_rebuild_rtv( LOWORD(lp), HIWORD(lp) );
        return 0;
    }
    return ::DefWindowProcW( hwnd, msg, wp, lp );
}

} // anonymous namespace

// ============================================================================
//  Public API
// ============================================================================
bool launcher_ui::run()
{
    // Reset state in case of multiple calls (shouldn't happen, but be safe)
    lc_step    = Step::Mode;
    lc_km_mode = false;
    lc_running = true;
    lc_dragging = false;
    lc_spin    = 0.f;
    lc_done    = false;
    lc_ok      = false;
    lc_msg.clear();
    lc_log.clear();

    const HINSTANCE hInst = ::GetModuleHandleW( nullptr );

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof( wc );
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = lc_wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = ::LoadCursorW( nullptr, MAKEINTRESOURCEW( 32512 ) ); // IDC_ARROW
    wc.lpszClassName = L"n4teyw4re.launcher_ui";
    ::RegisterClassExW( &wc );

    const int sx = ::GetSystemMetrics( SM_CXSCREEN );
    const int sy = ::GetSystemMetrics( SM_CYSCREEN );

    lc_hwnd = ::CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_LAYERED,
        wc.lpszClassName, L"N4TEYW4RE",
        WS_POPUP | WS_VISIBLE,
        ( sx - LC_W ) / 2, ( sy - LC_H ) / 2, LC_W, LC_H,
        nullptr, nullptr, hInst, nullptr );

    ::SetLayeredWindowAttributes( lc_hwnd, 0, 250, LWA_ALPHA );

    if ( !lc_create_device( lc_hwnd ) )
    {
        ::DestroyWindow( lc_hwnd );
        ::UnregisterClassW( wc.lpszClassName, hInst );
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext( ctx );

    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.f;
    st.FrameRounding  = 6.f;
    st.ItemSpacing    = ImVec2{8.f,6.f};
    st.FramePadding   = ImVec2{8.f,4.f};
    st.Colors[ImGuiCol_WindowBg] = pal::bg;
    st.Colors[ImGuiCol_Text]     = pal::text_hi;
    st.Colors[ImGuiCol_Border]   = pal::border;

    ImGui_ImplWin32_Init( lc_hwnd );
    ImGui_ImplDX11_Init( lc_dev, lc_ctx );

    ::ShowWindow( lc_hwnd, SW_SHOW );
    ::UpdateWindow( lc_hwnd );

    while ( lc_running )
    {
        MSG msg{};
        while ( ::PeekMessageW( &msg, nullptr, 0, 0, PM_REMOVE ) )
        {
            ::TranslateMessage( &msg );
            ::DispatchMessageW( &msg );
            if ( msg.message == WM_QUIT ) lc_running = false;
        }
        if ( !lc_running ) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        render_frame();
        ImGui::Render();

        constexpr float clear[4]{ 10/255.f, 9/255.f, 18/255.f, 1.f };
        lc_ctx->OMSetRenderTargets( 1, &lc_rtv, nullptr );
        lc_ctx->ClearRenderTargetView( lc_rtv, clear );
        ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData() );
        lc_sc->Present( 1, 0 );
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext( ctx );
    lc_cleanup_device();
    ::DestroyWindow( lc_hwnd );
    ::UnregisterClassW( wc.lpszClassName, hInst );

    return false; // caller should exit; the re-launched process is the real app
}
