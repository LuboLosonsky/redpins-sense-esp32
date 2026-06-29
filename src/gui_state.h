#pragma once
#include <stdint.h>

// Tlačidlá - zdieľané medzi gui.cpp (čítanie/debounce) a gui_screen_graph.cpp
// (okamžité prerušenie animácie grafu pri stlačení akéhokoľvek tlačidla).
#define BOOT_BUTTON_PIN 9
#define BTN_OK_PIN 2
#define BTN_ESC_PIN 3
#define BTN_UP_PIN 4
#define BTN_DOWN_PIN 5

// Plain aggregate (žiadne konštruktory, žiadne virtuály) - zdieľaný
// "kontext" medzi gui_task a všetkými gui_screen_*.cpp render funkciami.
// Stav tlačidiel (debounce) tu nie je - ten zostáva ako lokálne premenné
// v gui_task, lebo ho nečíta žiadna obrazovka.
struct GuiState {
    // --- Navigácia / stavový automat ---
    int current_screen;          // 0..5, index obrazovky
    bool is_in_options;          // je otvorené options menu?
    int selected_option_idx;     // kurzor v options menu
    int graph_range_days;        // 1 / 3 / 7 (možnosť na obrazovke GRAPH)
    int graph_flip_interval_ms;  // 15000 / 30000 (možnosť na obrazovke GRAPH)
    bool s_display_rotated;      // zrkadlí app_config_get()->display_rotated

    // --- Riadenie prekresľovania ---
    bool force_redraw;
    uint32_t last_draw_time;

    // --- Stav špecifický pre obrazovku GRAPH (čítaný aj animáciou
    //     bežiaceho bodu v status bare v gui_task) ---
    uint32_t last_graph_redraw_time;
    int graph_page;  // 0 = IN (lokálny senzor), 1 = OUT (OWM)
    int last_dot_x;  // animovaný progress bod v status bare
    uint32_t last_anim_sec;

    // --- Cache pre zabránenie zbytočnému prekresľovaniu (zdieľané medzi
    //     viacerými obrazovkami) ---
    float cache_t, cache_h;
    float cache_v_t, cache_v_h, cache_wt;
    int cache_wh, cache_wp, cache_wid;
    int cache_aqi;
    float cache_pm25;
    int cache_local_p;
    int cache_weather_sensor_ok;

    // --- Adaptívny jas (nezávislý od vybranej obrazovky) ---
    uint32_t last_brightness_update_ms;
    float filtered_lux;
};
